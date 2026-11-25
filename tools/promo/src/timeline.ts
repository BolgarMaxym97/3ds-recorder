export type Vec3 = [number, number, number];

// Camera on a sphere around the point between the two screens (it moves with the lid).
export type Orbit = {
	yaw: number; // degrees, 0 = straight in front, + = from the right
	elev: number; // degrees above the base
	dist: number; // cm
	lid: number; // lid angle, degrees (0 closed, 150 fully open)
};

export type Shot = {
	clip: string | null;
	dur: number; // frames
	from: Orbit;
	to: Orbit;
	lidFrames?: [number, number]; // frames the lid moves in (default: the whole shot)
	shift?: Vec3; // moves the camera target (e.g. to make room for the phone)
	spin?: [number, number]; // console rotation around Y, radians
	screensOnAt?: number; // frame the screens light up (default: lit from the start)
	caption?: string;
	footnote?: boolean; // show the asterisk note under the caption
	cut?: boolean; // hard cut with a flash instead of a fade through black
	phone?: boolean;
	hotkey?: {presses: number[]}; // beats into the shot where the hotkey is pressed (start, stop)
	replay?: {press: number}; // beat into the shot where the replay hotkey saves a clip
	sub?: string; // small line under the caption
};

// Our console space (see Real3DS): hinge axis at (0, 0.36, -3.35), bottom screen centre at (0, 0, 0),
// top screen centre 3.55 cm from the hinge along the lid.
const HINGE: Vec3 = [0, 0.36, -3.35];
const TOP_FROM_HINGE = 3.55;

export const screensMidpoint = (lid: number): Vec3 => {
	const a = (lid * Math.PI) / 180;
	const top: Vec3 = [0, HINGE[1] + TOP_FROM_HINGE * Math.sin(a), HINGE[2] + TOP_FROM_HINGE * Math.cos(a)];
	return [0, top[1] / 2, top[2] / 2];
};

// Scales every shot's distance: the whole console with both screens stays in frame.
const FRAMING = 1.3;

export const orbitCamera = (o: Orbit, shift: Vec3 = [0, 0, 0]): {pos: Vec3; look: Vec3} => {
	const m = screensMidpoint(o.lid);
	// a little below the midpoint, so the console sits above the captions
	const look: Vec3 = [m[0] + shift[0], m[1] - 0.35 + shift[1], m[2] + shift[2]];
	const y = (o.yaw * Math.PI) / 180;
	const e = (o.elev * Math.PI) / 180;
	const d = o.dist * FRAMING;
	return {
		look,
		pos: [
			look[0] + d * Math.sin(y) * Math.cos(e),
			look[1] + d * Math.sin(e),
			look[2] + d * Math.cos(y) * Math.cos(e),
		],
	};
};

// The music is 130 BPM with its first beat at frame 0.2 (measured from the kick drum's onsets).
// Shots are written in beats and turned into frames on that grid, so every cut lands on a beat.
export const FPS = 30;
const BEAT = (60 / 130) * FPS; // 13.85 frames
const FIRST_BEAT = 0.2;
const beatFrame = (beat: number) => Math.round(FIRST_BEAT + beat * BEAT);
const BARS2 = 8; // beats

const shot = (clip: string, from: Orbit, to: Orbit, caption: string, extra: Partial<Shot> = {}): Shot => ({
	clip,
	dur: BARS2,
	from,
	to,
	caption,
	...extra,
});

const montage = (clip: string, yaw: number, elev: number, lid: number): Shot => ({
	clip,
	dur: 2,
	from: {yaw, elev, dist: 22, lid},
	to: {yaw: yaw * 0.8, elev, dist: 19.5, lid},
	caption: 'NO PC. NO CAPTURE CARD.',
	cut: true,
});

// `dur` is in beats here; SHOTS below has it in frames.
const SHOTS_IN_BEATS: Shot[] = [
	{
		clip: 'home',
		dur: 12,
		from: {yaw: 0, elev: 58, dist: 30, lid: 0},
		to: {yaw: 12, elev: 36, dist: 24, lid: 120},
		lidFrames: [50, 112],
		spin: [-1.1, 0],
		screensOnAt: 112,
	},
	shot('mk7a', {yaw: -28, elev: 34, dist: 23, lid: 120}, {yaw: -12, elev: 38, dist: 21, lid: 125}, 'RECORD ANY GAME*', {
		footnote: true,
	}),
	shot('mk7b', {yaw: 38, elev: 30, dist: 22, lid: 125}, {yaw: 14, elev: 42, dist: 20, lid: 140}, 'UP TO 30 FPS'),
	// the console unfolds in front of the camera
	shot('smash', {yaw: 0, elev: 34, dist: 24, lid: 112}, {yaw: 0, elev: 56, dist: 19, lid: 150}, 'BOTH SCREENS. WITH SOUND.'),
	shot('homemenu', {yaw: 0, elev: 60, dist: 22, lid: 145}, {yaw: 0, elev: 50, dist: 19.5, lid: 145}, 'THE HOME MENU TOO'),
	// R + SELECT on the console: recording starts (LED breathes red), then stops (green twice).
	{
		clip: 'hotkey',
		dur: 16,
		from: {yaw: 58, elev: 24, dist: 15, lid: 115},
		to: {yaw: 30, elev: 32, dist: 19, lid: 115},
		shift: [1.8, -0.3, 0],
		hotkey: {presses: [3, 12]},
		caption: 'ONE HOTKEY. ANYWHERE.',
		sub: 'Any button combo you like — set it in Settings',
	},
	// Replay: the console keeps the last seconds all the time; the replay hotkey turns them into a clip.
	{
		clip: 'kirby',
		dur: 11, // the clip is 5 s
		from: {yaw: -34, elev: 30, dist: 24, lid: 125},
		to: {yaw: -16, elev: 36, dist: 23, lid: 125},
		shift: [3.6, 0, 0],
		replay: {press: 4},
		caption: 'MISSED IT? SAVE WHAT JUST HAPPENED.',
		sub: 'Replay keeps the last 15 s to 5 min, one hotkey makes it a clip',
	},
	shot('gallist', {yaw: -14, elev: 52, dist: 21, lid: 125}, {yaw: -4, elev: 48, dist: 19.5, lid: 125}, 'SAVED TO THE GALLERY', {dur: 6}),
	{
		clip: 'qr',
		dur: 16,
		from: {yaw: 8, elev: 30, dist: 26, lid: 120},
		to: {yaw: 18, elev: 27, dist: 24.5, lid: 120},
		shift: [4.2, 0.6, 0],
		phone: true,
		caption: 'SCAN. DOWNLOAD. SHARE.',
	},
	montage('mk7c', -35, 32, 128),
	montage('smash2', 30, 36, 128),
	montage('kirby2', 0, 55, 150),
	montage('mk7b', -12, 42, 138),
];

let beat = 0;
export const SHOTS: Shot[] = SHOTS_IN_BEATS.map((shot) => {
	const from = beatFrame(beat);
	const startBeat = beat;
	beat += shot.dur;
	const hotkey = shot.hotkey && {presses: shot.hotkey.presses.map((b) => beatFrame(startBeat + b) - from)};
	const replay = shot.replay && {press: beatFrame(startBeat + shot.replay.press) - from};
	return {...shot, dur: beatFrame(beat) - from, hotkey, replay};
});
export const OUTRO_START = beatFrame(beat);
export const OUTRO = beatFrame(beat + 13) - OUTRO_START;
export const TOTAL = OUTRO_START + OUTRO;
// Frames where a shot starts with a fade (for the whoosh sound).
export const FADE_CUTS: number[] = [];
{
	let at = 0;
	SHOTS.forEach((sh, i) => {
		if (i > 0 && !sh.cut) FADE_CUTS.push(at);
		at += sh.dur;
	});
	FADE_CUTS.push(OUTRO_START);
}
// From the first gameplay shot to the end of the montage: "recorded on a real Old 3DS" badge.
export const BADGE_FROM = SHOTS[0].dur;
