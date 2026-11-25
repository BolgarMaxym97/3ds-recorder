import {ThreeCanvas} from '@remotion/three';
import React from 'react';
import {AbsoluteFill, Sequence, useVideoConfig} from 'remotion';
import {fontFamily, ShotScene} from './Promo';
import type {Shot} from './timeline';

// GitHub social preview (1280x640): the console on the right with a game on its screens, the name on the left.
// Render one frame: npx remotion still src/index.ts Cover out/cover.png --frame=<n> --gl angle
export const COVER_CLIP_AT = 45; // frame of the clip shown on the screens

const SHOT: Shot = {
	clip: 'mk7a',
	dur: 1000,
	from: {yaw: -28, elev: 24, dist: 23, lid: 120},
	to: {yaw: -28, elev: 24, dist: 23, lid: 120},
	shift: [-6.3, 0.6, 0],
};

export const Cover: React.FC = () => {
	const {width, height} = useVideoConfig();
	return (
		<AbsoluteFill style={{background: '#000'}}>
			<ThreeCanvas width={width} height={height} camera={{fov: 30, near: 0.1, far: 200}}>
				<ShotScene shot={SHOT} />
			</ThreeCanvas>
			<AbsoluteFill style={{justifyContent: 'center', paddingLeft: 80, fontFamily, color: '#fff'}}>
				<div
					style={{
						fontWeight: 900,
						fontStyle: 'italic',
						fontSize: 92,
						lineHeight: 0.95,
						letterSpacing: 1,
						textShadow: '0 0 20px #3c9dff, 0 0 50px #1f6fff, 0 0 100px rgba(31,111,255,0.55)',
					}}
				>
					3DS
					<br />
					RECORDER
				</div>
				<div style={{fontWeight: 800, fontSize: 27, letterSpacing: 3, marginTop: 26, lineHeight: 1.35, maxWidth: 560}}>
					RECORD YOUR 3DS ON THE CONSOLE ITSELF
				</div>
				<div style={{fontWeight: 800, fontSize: 19, letterSpacing: 3, marginTop: 14, opacity: 0.75}}>
					NO CAPTURE CARD · NO PC
				</div>
				<div style={{fontWeight: 800, fontSize: 19, letterSpacing: 3, marginTop: 34, color: '#9cc8ff'}}>
					FREE · OPEN SOURCE HOMEBREW
				</div>
			</AbsoluteFill>
		</AbsoluteFill>
	);
};
