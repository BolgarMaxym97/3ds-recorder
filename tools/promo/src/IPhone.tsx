import {useGLTF} from '@react-three/drei';
import React, {useMemo} from 'react';
import {staticFile} from 'remotion';
import {Mesh, MeshBasicMaterial, PlaneGeometry, Texture} from 'three';
import {REC_H, REC_W} from './ScreenVideo';

// "Apple iPhone 13 Pro Max" by DatSketch (Sketchfab, CC-BY-4.0). In the file it is 0.99 tall,
// the display faces -z at z = -0.0243 and is 0.444 x 0.954.
const SRC = 'models/apple_iphone_13_pro_max/scene.gltf';
const SCREEN_W = 0.444;
const SCREEN_Z = 0.0243;
const SCREEN_H = 0.954;

// Phone with the display towards +z. `fill`: the texture is the phone's own screen recording and covers
// the display; otherwise it is the 400x480 recording shown like a video player.
export const IPhone: React.FC<{texture: Texture; height: number; fill?: boolean}> = ({texture, height, fill}) => {
	const {scene: original} = useGLTF(staticFile(SRC));
	const scene = useMemo(() => {
		const s = original.clone(true);
		const wallpaper = s.getObjectByName('Body_Wallpaper_0') as Mesh | undefined;
		if (wallpaper) wallpaper.material = new MeshBasicMaterial({color: '#000'});
		const glass = s.getObjectByName('Body.001_Screen Glass_0') as Mesh | undefined;
		if (glass) glass.visible = false; // its reflection washes out the video
		return s;
	}, [original]);
	const video = useMemo(() => new PlaneGeometry(SCREEN_W, fill ? SCREEN_H : (SCREEN_W * REC_H) / REC_W), [fill]);

	return (
		<group scale={height / 0.99}>
			<group rotation={[0, Math.PI, 0]}>
				<primitive object={scene} />
			</group>
			<mesh position={[0, 0, SCREEN_Z + 0.0005]}>
				<planeGeometry args={[SCREEN_W, SCREEN_H]} />
				<meshBasicMaterial color="#000" />
			</mesh>
			<mesh geometry={video} position={[0, fill ? 0 : 0.04, SCREEN_Z + 0.001]}>
				<meshBasicMaterial map={texture} toneMapped={false} />
			</mesh>
		</group>
	);
};
