import {useThree} from '@react-three/fiber';
import {Video} from '@remotion/media';
import React, {useCallback, useState} from 'react';
import {staticFile, useRemotionEnvironment} from 'remotion';
import {CanvasTexture, SRGBColorSpace} from 'three';

// Console recordings are 400x480: the top screen over the bottom one.
// prep.sh upscales the clips 4x, the texture keeps that resolution.
export const REC_W = 400;
export const REC_H = 480;
const TEX_W = REC_W * 4;
const TEX_H = REC_H * 4;

export const useClipTexture = (
	clip: string | null,
	size: [number, number] = [TEX_W, TEX_H],
): {texture: CanvasTexture<OffscreenCanvas>; video: React.ReactNode} => {
	const [w, h] = size;
	const [stuff] = useState(() => {
		const canvas = new OffscreenCanvas(w, h);
		const context = canvas.getContext('2d')!;
		context.fillStyle = '#000';
		context.fillRect(0, 0, w, h);
		const texture = new CanvasTexture(canvas);
		texture.colorSpace = SRGBColorSpace;
		texture.anisotropy = 8;
		return {context, texture};
	});
	const {invalidate, advance} = useThree();
	const {isRendering} = useRemotionEnvironment();

	const onVideoFrame = useCallback(
		(frame: CanvasImageSource) => {
			stuff.context.drawImage(frame, 0, 0, w, h);
			stuff.texture.needsUpdate = true;
			if (isRendering) {
				advance(performance.now());
			} else {
				invalidate();
			}
		},
		[stuff, invalidate, advance, isRendering, w, h],
	);

	const video = clip ? (
		<Video src={staticFile(`clips/${clip}.mp4`)} onVideoFrame={onVideoFrame} muted headless />
	) : null;
	return {texture: stuff.texture, video};
};
