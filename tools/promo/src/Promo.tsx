import {useThree} from '@react-three/fiber';
import {Bloom, EffectComposer, ToneMapping, Vignette} from '@react-three/postprocessing';
import {ToneMappingMode} from 'postprocessing';
import {Audio} from '@remotion/media';
import {loadFont} from '@remotion/google-fonts/Montserrat';
import {CameraMotionBlur} from '@remotion/motion-blur';
import {ThreeCanvas} from '@remotion/three';
import React, {useEffect} from 'react';
import {
	AbsoluteFill,
	Easing,
	interpolate,
	Sequence,
	spring,
	staticFile,
	useCurrentFrame,
	useVideoConfig,
} from 'remotion';
import {Color, DoubleSide, Mesh, MeshBasicMaterial, PlaneGeometry, PMREMGenerator, Scene, Vector3} from 'three';
import {IPhone} from './IPhone';
import {Real3DS, type Hotkey} from './Real3DS';
import {useClipTexture} from './ScreenVideo';
import {BADGE_FROM, FADE_CUTS, orbitCamera, OUTRO, OUTRO_START, SHOTS, TOTAL, type Orbit, type Shot, type Vec3} from './timeline';

export const {fontFamily} = loadFont('italic', {weights: ['800', '900'], subsets: ['latin']});

const ease = Easing.inOut(Easing.cubic);
const lerp = (a: number, b: number, t: number) => a + (b - a) * t;

// A dark studio with soft boxes: glossy black stays black, the shell gets clean highlight strips.
const studioEnvironment = () => {
	const scene = new Scene();
	scene.background = new Color('#000');
	const box = (w: number, h: number, pos: Vec3, look: Vec3, power: number, tint = '#ffffff') => {
		const m = new Mesh(new PlaneGeometry(w, h), new MeshBasicMaterial({color: new Color(tint).multiplyScalar(power), side: DoubleSide}));
		m.position.set(...pos);
		m.lookAt(...look);
		scene.add(m);
	};
	box(12, 5, [0, 12, -6], [0, 0, 0], 2.4); // overhead, behind: the lid's black face doesn't mirror it
	box(3, 12, [-12, 5, 6], [0, 2, 0], 3); // key strip, left
	box(3, 12, [12, 4, -4], [0, 2, 0], 2.4, '#bcd8ff'); // rim strip, right back
	box(16, 3, [0, 2, 16], [0, 2, 0], 0.5); // low front fill
	return scene;
};

const Lights: React.FC = () => {
	const {gl, scene} = useThree();
	useEffect(() => {
		const pmrem = new PMREMGenerator(gl);
		const env = pmrem.fromScene(studioEnvironment(), 0.02).texture;
		scene.environment = env;
		scene.environmentIntensity = 1;
		return () => {
			env.dispose();
			pmrem.dispose();
		};
	}, [gl, scene]);
	return (
		<>
			<ambientLight intensity={0.12} />
			<directionalLight position={[-8, 14, 10]} intensity={1.6} />
			<directionalLight position={[10, 6, -12]} intensity={2.2} color="#8fc0ff" />
			<spotLight position={[0, 5, -18]} angle={0.6} intensity={110} color="#a8d0ff" />
		</>
	);
};

// Glow around bright screen content, neutral tone mapping (keeps the recordings' colours), soft vignette.
const Effects: React.FC = () => (
	<EffectComposer multisampling={8}>
		<Bloom mipmapBlur intensity={0.3} luminanceThreshold={0.92} luminanceSmoothing={0.08} />
		<ToneMapping mode={ToneMappingMode.NEUTRAL} />
		<Vignette offset={0.3} darkness={0.55} />
	</EffectComposer>
);

// Bottom of the closed console in our space (the phone stands on the same level).
const CONSOLE_BOTTOM = -1.02;

const CameraRig: React.FC<{pos: Vec3; look: Vec3}> = ({pos, look}) => {
	const {camera} = useThree();
	camera.position.set(...pos);
	camera.lookAt(new Vector3(...look));
	camera.updateMatrixWorld();
	return null;
};

// The phone that receives the MP4: shows the recording as a whole (both screens).
const Phone: React.FC<{x: number}> = ({x}) => {
	const {texture, video} = useClipTexture('phonerec', [720, 1566]);
	return (
		<group position={[x, CONSOLE_BOTTOM + 4.75, 0.5]} rotation={[0, -0.38, 0]}>
			{video}
			<IPhone texture={texture} height={9.5} fill />
		</group>
	);
};

// R + SELECT pressed at `presses` (frames): the first starts recording (the LED breathes red, as the
// recorder does), the second saves it (the LED blinks green twice).
const hotkeyState = (f: number, presses: number[]): Hotkey => {
	const [start, stop] = presses;
	const press = Math.max(...presses.map((p) => interpolate(f, [p - 3, p, p + 5, p + 9], [0, 1, 1, 0], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'})));
	let led: [number, number, number] = [0, 0, 0];
	if (f >= start + 2 && f < stop + 2) {
		const breath = 0.25 + 0.75 * (0.5 - 0.5 * Math.cos(((f - start - 2) / 45) * Math.PI * 2));
		led = [breath, 0.02 * breath, 0.02 * breath];
	} else if (f >= stop + 2) {
		const t = f - stop - 2;
		const on = (t < 7) || (t >= 13 && t < 20);
		led = on ? [0.05, 1, 0.25] : [0, 0, 0];
	}
	return {press, led};
};

// Replay: the LED glows faint blue while the last seconds are kept; the replay hotkey saves them
// (green twice), as the recorder does.
const replayState = (f: number, press: number): Hotkey => {
	const pressed = interpolate(f, [press - 3, press, press + 5, press + 9], [0, 1, 1, 0], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'});
	let led: [number, number, number] = [0.02, 0.1, 0.45];
	if (f >= press + 2) {
		const t = f - press - 2;
		const on = t < 7 || (t >= 13 && t < 20);
		led = on ? [0.05, 1, 0.25] : t >= 20 ? [0.02, 0.1, 0.45] : [0, 0, 0];
	}
	return {press: pressed, led, r: false};
};

// Replay on screen: a timeline of the last seconds; on the hotkey it sweeps back and becomes a clip.
const ReplayOverlay: React.FC<{press: number}> = ({press}) => {
	const f = useCurrentFrame();
	const {fps} = useVideoConfig();
	const W = 520;
	const intro = spring({frame: f - 4, fps, config: {damping: 16}});
	const sweep = interpolate(f, [press + 2, press + 22], [0, 1], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp', easing: Easing.out(Easing.cubic)});
	const chipIn = spring({frame: f - press + 4, fps, config: {damping: 14, mass: 0.6}});
	const chipOut = interpolate(f, [press + 22, press + 30], [1, 0], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'});
	const saved = spring({frame: f - press - 22, fps, config: {damping: 14, mass: 0.6}});
	const label = {fontFamily, fontWeight: 900, fontSize: 26, letterSpacing: 4, color: '#fff', textShadow: '0 3px 12px rgba(0,0,0,0.9)'} as const;
	return (
		<AbsoluteFill style={{alignItems: 'flex-end', justifyContent: 'flex-start', padding: '170px 120px'}}>
			<div style={{display: 'flex', flexDirection: 'column', gap: 20, opacity: intro}}>
				<div style={{display: 'flex', alignItems: 'center', gap: 12, ...label}}>
					<div style={{width: 18, height: 18, borderRadius: 9, background: '#4a7dff', boxShadow: '0 0 14px #4a7dff'}} />
					REPLAY · LAST 30 S KEPT
				</div>
				<div style={{position: 'relative', width: W, height: 16, borderRadius: 8, background: 'rgba(255,255,255,0.14)', overflow: 'hidden'}}>
					{/* the kept seconds, always moving */}
					<div style={{position: 'absolute', inset: 0, background: 'repeating-linear-gradient(90deg, rgba(120,160,255,0.55) 0 22px, rgba(120,160,255,0.25) 22px 44px)', backgroundPositionX: -f * 3}} />
					{/* the clip: sweeps back from now */}
					<div style={{position: 'absolute', top: 0, bottom: 0, right: 0, width: W * sweep, background: 'linear-gradient(90deg, #34c759, #8ef0a5)', boxShadow: '0 0 18px #34c759'}} />
				</div>
				<div style={{display: 'flex', justifyContent: 'space-between', width: W, ...label, fontSize: 20, opacity: 0.8}}>
					<span>−30 S</span>
					<span>NOW</span>
				</div>
				<div style={{height: 76, position: 'relative'}}>
					<div style={{position: 'absolute', opacity: Math.min(chipIn, chipOut), transform: `scale(${0.7 + 0.3 * chipIn})`, transformOrigin: 'left center', padding: '10px 22px', borderRadius: 16, background: 'rgba(255,255,255,0.94)', color: '#0b0d10', fontFamily, fontWeight: 900, fontSize: 52}}>
						HOTKEY
					</div>
					<div style={{position: 'absolute', top: 12, display: 'flex', alignItems: 'center', gap: 12, opacity: saved, ...label, fontSize: 44}}>
						<div style={{width: 24, height: 24, borderRadius: 12, background: '#34c759', boxShadow: '0 0 16px #34c759'}} />
						CLIP SAVED
					</div>
				</div>
			</div>
		</AbsoluteFill>
	);
};

// The keys on screen while they are pressed, then REC / SAVED.
const HotkeyOverlay: React.FC<{presses: number[]}> = ({presses}) => {
	const f = useCurrentFrame();
	const {fps} = useVideoConfig();
	const [start, stop] = presses;
	const chip = (label: string, wide = false) => (
		<div style={{padding: '10px 22px', minWidth: wide ? 0 : 64, textAlign: 'center', borderRadius: 16, background: 'rgba(255,255,255,0.94)', color: '#0b0d10', fontFamily, fontWeight: 900, fontSize: 58, boxShadow: '0 6px 30px rgba(0,0,0,0.6)'}}>
			{label}
		</div>
	);
	const pop = (at: number) => spring({frame: f - at, fps, config: {damping: 14, mass: 0.6}});
	const keys = (at: number) => {
		const p = pop(at - 4);
		const out = interpolate(f, [at + 22, at + 30], [1, 0], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'});
		return (
			<div style={{display: 'flex', alignItems: 'center', gap: 18, opacity: Math.min(p, out), transform: `scale(${0.7 + 0.3 * p})`}}>
				{chip('HOTKEY', true)}
			</div>
		);
	};
	const tag = (at: number, text: string, color: string, until: number) => {
		const p = pop(at);
		const out = interpolate(f, [until - 8, until], [1, 0], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'});
		return (
			<div style={{display: 'flex', alignItems: 'center', gap: 12, opacity: Math.min(p, out), fontFamily, fontWeight: 900, fontSize: 48, letterSpacing: 4, color: '#fff', textShadow: '0 3px 14px rgba(0,0,0,0.9)'}}>
				<div style={{width: 26, height: 26, borderRadius: 13, background: color, boxShadow: `0 0 16px ${color}`}} />
				{text}
			</div>
		);
	};
	return (
		<AbsoluteFill style={{alignItems: 'flex-start', justifyContent: 'flex-start', padding: '150px 130px'}}>
			<div style={{display: 'flex', flexDirection: 'column', alignItems: 'flex-start', gap: 26}}>
				{f < start + 32 ? keys(start) : f >= stop - 6 && f < stop + 32 ? keys(stop) : null}
				{f >= start + 6 && f < stop ? tag(start + 6, 'REC', '#ff3b30', stop) : null}
				{f >= stop + 6 ? tag(stop + 6, 'SAVED', '#34c759', 10000) : null}
			</div>
		</AbsoluteFill>
	);
};

export const ShotScene: React.FC<{shot: Shot; brightnessScale?: number}> = ({shot, brightnessScale = 1}) => {
	const frame = useCurrentFrame();
	const {fps} = useVideoConfig();
	const t = shot.cut ? frame / shot.dur : ease(Math.min(1, frame / shot.dur));
	const [l0, l1] = shot.lidFrames ?? [0, shot.dur];
	const lt = interpolate(frame, [l0, l1], [0, 1], {
		extrapolateLeft: 'clamp',
		extrapolateRight: 'clamp',
		easing: Easing.inOut(Easing.quad),
	});
	const lid = lerp(shot.from.lid, shot.to.lid, lt);
	const orbit: Orbit = {
		yaw: lerp(shot.from.yaw, shot.to.yaw, t),
		elev: lerp(shot.from.elev, shot.to.elev, t),
		dist: lerp(shot.from.dist, shot.to.dist, t),
		lid,
	};
	const cam = orbitCamera(orbit, shot.shift);
	const hotkey = shot.hotkey ? hotkeyState(frame, shot.hotkey.presses) : shot.replay ? replayState(frame, shot.replay.press) : undefined;
	const spin = shot.spin
		? interpolate(frame, [0, shot.dur * 0.7], shot.spin, {extrapolateRight: 'clamp', easing: Easing.out(Easing.cubic)})
		: 0;
	const on = shot.screensOnAt;
	const brightness =
		(on === undefined ? 1 : interpolate(frame, [on, on + 14], [0, 1], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'})) *
		brightnessScale;
	const {texture, video} = useClipTexture(shot.clip);
	const phoneIn = shot.phone ? spring({frame: frame - 55, fps, config: {damping: 16, mass: 0.8}}) : 0;

	return (
		<>
			{video}
			<Lights />
			<Effects />
			<CameraRig pos={cam.pos} look={cam.look} />
			<group rotation={[0, spin, 0]}>
				<Real3DS lidAngle={lid} texture={texture} brightness={brightness} hotkey={hotkey} />
			</group>
			{shot.phone ? <Phone x={interpolate(phoneIn, [0, 1], [26, 11])} /> : null}
		</>
	);
};

const Fade: React.FC<{dur: number; fadeIn: boolean; fadeOut: boolean; flash: boolean}> = (p) => {
	const f = useCurrentFrame();
	const black = Math.max(
		p.fadeIn ? interpolate(f, [0, 8], [1, 0], {extrapolateRight: 'clamp'}) : 0,
		p.fadeOut ? interpolate(f, [p.dur - 8, p.dur], [0, 1], {extrapolateLeft: 'clamp'}) : 0,
	);
	const white = p.flash ? interpolate(f, [0, 5], [0.3, 0], {extrapolateRight: 'clamp'}) : 0;
	return (
		<>
			<AbsoluteFill style={{background: '#fff', opacity: white}} />
			<AbsoluteFill style={{background: '#000', opacity: black}} />
		</>
	);
};

const Caption: React.FC<{text: string; dur: number; sub?: string}> = ({text, dur, sub}) => {
	const f = useCurrentFrame();
	const opacity = interpolate(f, [0, 8, dur - 8, dur], [0, 1, 1, 0], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'});
	const y = interpolate(f, [0, 10], [18, 0], {extrapolateRight: 'clamp', easing: Easing.out(Easing.cubic)});
	return (
		<AbsoluteFill style={{justifyContent: 'flex-end', alignItems: 'center', paddingBottom: 64}}>
			<div
				style={{
					fontFamily,
					fontWeight: 900,
					fontStyle: 'italic',
					fontSize: 62,
					letterSpacing: 1,
					color: '#fff',
					opacity,
					transform: `translateY(${y}px)`,
					textShadow: '0 4px 18px rgba(0,0,0,0.9)',
				}}
			>
				{text}
			</div>
			{sub ? (
				<div style={{fontFamily, fontWeight: 700, fontSize: 24, letterSpacing: 1, color: '#fff', opacity: opacity * 0.85, marginTop: 8, textShadow: '0 2px 10px rgba(0,0,0,0.9)'}}>
					{sub}
				</div>
			) : null}
		</AbsoluteFill>
	);
};

// Corner tag over the gameplay: everything shown was captured on real hardware.
const Badge: React.FC<{dur: number}> = ({dur}) => {
	const f = useCurrentFrame();
	const opacity = interpolate(f, [0, 12, dur - 10, dur], [0, 1, 1, 0], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'});
	const blink = 0.55 + 0.45 * Math.cos((f / 30) * Math.PI * 2);
	return (
		<AbsoluteFill style={{padding: '44px 56px', opacity}}>
			<div style={{display: 'flex', alignItems: 'center', gap: 14, fontFamily, fontWeight: 800, fontSize: 21, letterSpacing: 4, color: '#fff'}}>
				<div style={{width: 14, height: 14, borderRadius: 7, background: '#ff3b30', opacity: blink, boxShadow: '0 0 12px #ff3b30'}} />
				<span style={{textShadow: '0 2px 10px rgba(0,0,0,0.9)'}}>RECORDED ON A REAL 3DS</span>
			</div>
		</AbsoluteFill>
	);
};

export const FOOTNOTE = '* DS games are not recorded yet';

const Footnote: React.FC<{dur: number}> = ({dur}) => {
	const f = useCurrentFrame();
	const opacity = interpolate(f, [0, 8, dur - 8, dur], [0, 0.75, 0.75, 0], {
		extrapolateLeft: 'clamp',
		extrapolateRight: 'clamp',
	});
	return (
		<AbsoluteFill style={{justifyContent: 'flex-end', alignItems: 'center', paddingBottom: 26}}>
			<div style={{fontFamily, fontWeight: 800, fontSize: 18, letterSpacing: 1, color: '#fff', opacity}}>{FOOTNOTE}</div>
		</AbsoluteFill>
	);
};

// Consecutive shots with the same caption share one caption.
const captionSpans = () => {
	const spans: {text: string; from: number; dur: number; footnote: boolean; sub?: string}[] = [];
	let at = 0;
	for (const s of SHOTS) {
		const last = spans[spans.length - 1];
		if (s.caption && last && last.text === s.caption && last.from + last.dur === at) {
			last.dur += s.dur;
		} else if (s.caption) {
			spans.push({text: s.caption, from: at, dur: s.dur, footnote: !!s.footnote, sub: s.sub});
		}
		at += s.dur;
	}
	return spans;
};

const OutroText: React.FC = () => {
	const f = useCurrentFrame();
	const {fps} = useVideoConfig();
	const pop = spring({frame: f - 12, fps, config: {damping: 13}});
	const sub = interpolate(f, [34, 50], [0, 1], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'});
	const link = interpolate(f, [56, 72], [0, 1], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'});
	const out = interpolate(f, [OUTRO - 20, OUTRO], [1, 0], {extrapolateLeft: 'clamp'});
	return (
		<AbsoluteFill style={{justifyContent: 'center', alignItems: 'center', fontFamily, color: '#fff', opacity: out}}>
			<div
				style={{
					fontWeight: 900,
					fontStyle: 'italic',
					fontSize: 150,
					letterSpacing: 2,
					opacity: pop,
					transform: `scale(${0.8 + 0.2 * pop})`,
					textShadow: '0 0 24px #3c9dff, 0 0 60px #1f6fff, 0 0 120px rgba(31,111,255,0.6)',
				}}
			>
				3DS RECORDER
			</div>
			<div style={{fontWeight: 800, fontSize: 28, letterSpacing: 9, marginTop: 6, opacity: sub}}>
				RECORD EVERYTHING ON YOUR NINTENDO 3DS*
			</div>
			<div style={{fontWeight: 800, fontSize: 22, letterSpacing: 5, marginTop: 30, opacity: sub}}>
				ALL CLIPS RECORDED ON A REAL 3DS · NO EMULATOR
			</div>
			<div style={{fontWeight: 800, fontSize: 24, letterSpacing: 4, marginTop: 30, opacity: link, color: '#9cc8ff'}}>
				FREE · OPEN SOURCE · github.com/BolgarMaxym97/3ds-recorder
			</div>
		</AbsoluteFill>
	);
};

const OutroScene: React.FC = () => {
	const shot: Shot = {
		clip: 'home',
		dur: OUTRO,
		from: {yaw: 25, elev: 22, dist: 34, lid: 120},
		to: {yaw: -10, elev: 18, dist: 31, lid: 120},
		shift: [0, 5.5, 0],
	};
	return <ShotScene shot={shot} brightnessScale={0.35} />;
};

export const Promo: React.FC = () => {
	const {width, height} = useVideoConfig();
	let at = 0;
	const shots = SHOTS.map((shot, i) => {
		const from = at;
		at += shot.dur;
		const next = SHOTS[i + 1];
		return (
			<Sequence key={i} from={from} durationInFrames={shot.dur}>
				{shot.cut ? (
					<CameraMotionBlur samples={5} shutterAngle={180}>
						<ThreeCanvas width={width} height={height} camera={{fov: 30, near: 0.1, far: 200}}>
							<ShotScene shot={shot} />
						</ThreeCanvas>
					</CameraMotionBlur>
				) : (
					<ThreeCanvas width={width} height={height} camera={{fov: 30, near: 0.1, far: 200}}>
						<ShotScene shot={shot} />
					</ThreeCanvas>
				)}
				{shot.hotkey ? <HotkeyOverlay presses={shot.hotkey.presses} /> : null}
				{shot.replay ? <ReplayOverlay press={shot.replay.press} /> : null}
				<Fade dur={shot.dur} fadeIn={!shot.cut} fadeOut={!next?.cut} flash={!!shot.cut} />
			</Sequence>
		);
	});

	return (
		<AbsoluteFill style={{background: '#000'}}>
			{shots}
			<Sequence from={OUTRO_START} durationInFrames={OUTRO}>
				<AbsoluteFill style={{opacity: 0.4}}>
					<ThreeCanvas width={width} height={height} camera={{fov: 30, near: 0.1, far: 200}}>
						<OutroScene />
					</ThreeCanvas>
				</AbsoluteFill>
				<Fade dur={OUTRO} fadeIn fadeOut={false} flash={false} />
				<OutroText />
				<Sequence from={34}>
					<Footnote dur={OUTRO - 34} />
				</Sequence>
			</Sequence>
			<Sequence from={BADGE_FROM} durationInFrames={OUTRO_START - BADGE_FROM}>
				<Badge dur={OUTRO_START - BADGE_FROM} />
			</Sequence>
			{captionSpans().map((c) => (
				<Sequence key={c.from} from={c.from} durationInFrames={c.dur}>
					<Caption text={c.text} dur={c.dur} sub={c.sub} />
					{c.footnote ? <Footnote dur={c.dur} /> : null}
				</Sequence>
			))}
			{FADE_CUTS.map((f) => (
				<Sequence key={`w${f}`} from={Math.max(0, f - 9)} durationInFrames={24}>
					<Audio src={staticFile('whoosh.wav')} volume={0.35} />
				</Sequence>
			))}
			<Audio
				src={staticFile('music.m4a')}
				volume={(f) => interpolate(f, [0, 10, TOTAL - 45, TOTAL], [0, 0.9, 0.9, 0], {extrapolateRight: 'clamp'})}
			/>
		</AbsoluteFill>
	);
};
