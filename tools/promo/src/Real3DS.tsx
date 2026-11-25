import {useGLTF} from '@react-three/drei';
import {createPortal, useThree} from '@react-three/fiber';
import React, {useMemo} from 'react';
import {staticFile} from 'remotion';
import {Box3, Group, Matrix4, Object3D, Mesh, MeshPhysicalMaterial, MeshStandardMaterial, PlaneGeometry, Texture, Vector3} from 'three';
import {REC_W} from './ScreenVideo';

// "Nintendo 3DS" by Broeach8972 (Sketchfab, CC-BY-4.0). In the model's own space the base lies flat,
// its front faces -z and the lid leans back towards +z. Each mesh is in the file twice (Object_4..20
// and a copy, Object_21..37): the copies are hidden.
const SRC = 'models/nintendo_3ds/scene.gltf';
const LID_PARTS = ['Object_18', 'Object_19', 'Object_20'];
const INNER_PARTS = ['Object_18'];
const LID_SHELL = 'Object_19'; // lid shell, its inner side frames the top screen
const HIDDEN = Array.from({length: 17}, (_, i) => `Object_${21 + i}`);

// Measured in the model's space (see ModelTest).
export const MODEL = {
	hinge: new Vector3(0, 1.23, 0.05),
	lidNow: 150, // degrees the lid is open in the file
	scale: 13.4 / 12.3, // model is 12.3 wide, a real Old 3DS 13.4 cm
	bottomCenter: new Vector3(0, 0.9, -3.05),
	lidNormal: new Vector3(0, 0.864, -0.503),
	topCenter: new Vector3(0, 3.05, 2.75),
	screenH: 4.608 / (13.4 / 12.3),
	topW: 7.68 / (13.4 / 12.3),
	botW: 6.144 / (13.4 / 12.3),
};

const screenGeometry = (w: number, h: number, u0: number, u1: number, v0: number, v1: number) => {
	const g = new PlaneGeometry(w, h);
	const uv = g.attributes.uv;
	for (let i = 0; i < uv.count; i++) {
		uv.setXY(i, u0 + uv.getX(i) * (u1 - u0), v0 + uv.getY(i) * (v1 - v0));
	}
	return g;
};

const ScreenMesh: React.FC<{geometry: PlaneGeometry; texture: Texture; brightness: number; matrix: Matrix4}> = ({
	geometry,
	texture,
	brightness,
	matrix,
}) => (
	<group matrix={matrix} matrixAutoUpdate={false}>
		<mesh geometry={geometry}>
			<meshBasicMaterial map={texture} color={[brightness, brightness, brightness]} toneMapped={false} />
		</mesh>
	</group>
);

export type Hotkey = {
	press: number; // 0..1, how far R and SELECT are pushed in
	led: [number, number, number]; // notification LED colour, 0 = off
	r?: boolean; // the combo includes the R shoulder button (pressed in too)
};

// Model space positions (see ModelTest): the R button is Object_11; SELECT sits left under the bottom
// screen; the notification LED is at the right end of the hinge.
const R_BUTTON = 'Object_11';
const X_SLOT_HAS_Y = 'Object_12';
const Y_SLOT_HAS_X = 'Object_13';
// SELECT's label centre in our space is (-2.18, 0.06, 3.0), measured top-down with a grid (ModelTest).
const SELECT_POS = new Vector3(2.0, 0.96, -5.78);
const LED_POS = new Vector3(-5.95, 1.23, 0.05);

export const Real3DS: React.FC<{lidAngle: number; texture: Texture; brightness: number; hotkey?: Hotkey}> = ({
	lidAngle,
	texture,
	brightness,
	hotkey,
}) => {
	const {scene: original} = useGLTF(staticFile(SRC));

	const environment = useThree((st) => st.scene.environment);
	const {scene, pivot, dimmed, rButton, rRest} = useMemo(() => {
		const dimmed: MeshPhysicalMaterial[] = [];
		const scene = original.clone(true);
		scene.updateMatrixWorld(true);
		HIDDEN.forEach((n) => {
			const o = scene.getObjectByName(n);
			if (o) o.visible = false;
		});
		// Shinier plastic than the file's phong materials.
		scene.traverse((o) => {
			const m = o as Mesh;
			if (!m.isMesh) return;
			const old = m.material as MeshStandardMaterial;
			const inner = INNER_PARTS.includes(m.name);
			m.material = new MeshPhysicalMaterial({
				map: old.map,
				// the lid's inner face is glossy black on a real Old 3DS
				color: inner ? old.color.clone().multiplyScalar(0.06) : old.color.clone().multiplyScalar(0.85),
				roughness: inner ? 0.5 : 0.32,
				metalness: 0.1,
				clearcoat: inner ? 0.2 : 0.7,
				clearcoatRoughness: 0.1,
				// a softer mirror on the black face: the studio's soft boxes stay a hint, not a grey wedge
				side: old.side,
			});
			if (inner || m.name === LID_SHELL) dimmed.push(m.material as MeshPhysicalMaterial);
		});
		const pivot = new Group();
		pivot.position.copy(MODEL.hinge);
		scene.add(pivot);
		pivot.updateMatrixWorld(true);
		LID_PARTS.forEach((n) => {
			const o = scene.getObjectByName(n);
			if (o) pivot.attach(o);
		});
		// The file has the X and Y buttons the wrong way round (Y on top): swap them, as on a real 3DS.
		const top = scene.getObjectByName(X_SLOT_HAS_Y);
		const left = scene.getObjectByName(Y_SLOT_HAS_X);
		if (top && left) {
			scene.updateMatrixWorld(true);
			const ct = new Box3().setFromObject(top).getCenter(new Vector3());
			const cl = new Box3().setFromObject(left).getCenter(new Vector3());
			const move = (o: Object3D, from: Vector3, to: Vector3) => {
				const p = o.getWorldPosition(new Vector3()).add(to.clone().sub(from));
				o.position.copy(o.parent!.worldToLocal(p));
			};
			move(top, ct, cl);
			move(left, cl, ct);
		}
		const rButton = scene.getObjectByName(R_BUTTON);
		return {scene, pivot, dimmed, rButton, rRest: rButton?.position.clone()};
	}, [original]);

	// A softer mirror on the lid's black face: the studio's soft boxes stay a hint, not a grey wedge.
	// envMapIntensity only counts with the map set on the material itself (not via scene.environment).
	for (const m of dimmed) {
		if (m.envMap !== environment) {
			m.envMap = environment;
			m.envMapIntensity = 0.35;
			m.needsUpdate = true;
		}
	}

	pivot.rotation.x = ((lidAngle - MODEL.lidNow) * Math.PI) / 180;
	if (rButton && rRest) rButton.position.set(rRest.x, rRest.y - 0.12 * (hotkey?.r === false ? 0 : (hotkey?.press ?? 0)), rRest.z);
	const led = hotkey?.led ?? [0, 0, 0];
	const ledOn = Math.max(...led);

	const {topGeo, botGeo, topMatrix, botMatrix} = useMemo(() => {
		const topGeo = screenGeometry(MODEL.topW, MODEL.screenH, 0, 1, 0.5, 1);
		const botGeo = screenGeometry(MODEL.botW, MODEL.screenH, 40 / REC_W, 360 / REC_W, 0, 0.5);
		// Bottom screen: faces +y, image top towards the hinge (+z).
		const botMatrix = new Matrix4()
			.makeBasis(new Vector3(-1, 0, 0), new Vector3(0, 0, 1), new Vector3(0, 1, 0))
			.setPosition(MODEL.bottomCenter.clone().add(new Vector3(0, 0.012, 0)));
		// Top screen: on the lid's inner face, in the pivot's space (lid as it is in the file).
		const n = MODEL.lidNormal.clone().normalize();
		const up = new Vector3(0, n.z * -1, n.y).normalize(); // along the lid, away from the hinge
		const right = new Vector3().crossVectors(up, n);
		const topMatrix = new Matrix4()
			.makeBasis(right, up, n)
			.setPosition(MODEL.topCenter.clone().add(n.clone().multiplyScalar(0.05)).sub(MODEL.hinge));
		return {topGeo, botGeo, topMatrix, botMatrix};
	}, []);

	// Our console space: front towards +z, base top at y = 0, sized in cm.
	return (
		<group scale={MODEL.scale} rotation={[0, Math.PI, 0]} position={[0, -MODEL.bottomCenter.y * MODEL.scale, -3.3]}>
			<primitive object={scene} />
			{hotkey ? (
				<>
					{/* SELECT lights up while pressed */}
					<mesh position={SELECT_POS} rotation={[-Math.PI / 2, 0, 0]}>
						<planeGeometry args={[1.3, 0.42]} />
						<meshBasicMaterial color="#9fd0ff" transparent opacity={0.85 * hotkey.press} toneMapped={false} depthWrite={false} />
					</mesh>
					<mesh position={LED_POS}>
						<sphereGeometry args={[0.22, 24, 16]} />
						<meshBasicMaterial color={led.map((c) => 0.08 + c * 3) as [number, number, number]} toneMapped={false} />
					</mesh>
					{ledOn > 0.02 ? <pointLight position={LED_POS} color={led as [number, number, number]} intensity={ledOn * 6} distance={4} /> : null}
				</>
			) : null}
			<ScreenMesh geometry={botGeo} texture={texture} brightness={brightness} matrix={botMatrix} />
			{createPortal(<ScreenMesh geometry={topGeo} texture={texture} brightness={brightness} matrix={topMatrix} />, pivot)}
		</group>
	);
};
