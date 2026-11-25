import {ThreeCanvas} from '@remotion/three';
import React from 'react';
import {useVideoConfig} from 'remotion';
import {Real3DS} from './Real3DS';
import {useClipTexture} from './ScreenVideo';

// Scratch composition: top-down views with a 0.5 cm marker grid (our space) for placing parts.
const Grid: React.FC<{x0: number; x1: number; z0: number; z1: number}> = ({x0, x1, z0, z1}) => {
	const dots = [];
	for (let x = x0; x <= x1 + 1e-6; x += 0.5)
		for (let z = z0; z <= z1 + 1e-6; z += 0.5) {
			const major = Math.abs(x % 1) < 1e-6 && Math.abs(z % 1) < 1e-6;
			dots.push(
				<mesh key={`${x},${z}`} position={[x, 0.35, z]}>
					<sphereGeometry args={[major ? 0.06 : 0.03, 8, 8]} />
					<meshBasicMaterial color={major ? '#ff00ff' : '#ffff00'} />
				</mesh>,
			);
		}
	return <>{dots}</>;
};

const Inner: React.FC = () => {
	const {texture, video} = useClipTexture('hotkey');
	return (
		<>
			{video}
			<ambientLight intensity={1.2} />
			<directionalLight position={[0, 14, 4]} intensity={2} />
			<Real3DS lidAngle={150} texture={texture} brightness={1} hotkey={{press: 1, led: [0, 0, 0], r: false}} />
		</>
	);
};

export const ModelTest: React.FC<{view: number; lid: number}> = ({view}) => {
	const {width, height} = useVideoConfig();
	const views = [
		{pos: [-2, 14, 2.6] as [number, number, number], look: [-2, 0, 2.6] as [number, number, number], grid: [-4, 0, 1, 4]},
		{pos: [4.5, 14, -1] as [number, number, number], look: [4.5, 0, -1] as [number, number, number], grid: [2, 7, -3, 1]},
	];
	const v = views[view];
	return (
		<ThreeCanvas width={width} height={height} camera={{fov: 30, position: v.pos, up: [0, 0, -1]}} onCreated={({camera}) => camera.lookAt(...v.look)} style={{background: '#223'}}>
			<Inner />
			<Grid x0={v.grid[0]} x1={v.grid[1]} z0={v.grid[2]} z1={v.grid[3]} />
		</ThreeCanvas>
	);
};
