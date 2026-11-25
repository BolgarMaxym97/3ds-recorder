import React from 'react';
import {Composition} from 'remotion';
import {ModelTest} from './ModelTest';
import {Cover} from './Cover';
import {Promo} from './Promo';
import {TOTAL} from './timeline';

export const Root: React.FC = () => (
	<>
		<Composition id="Promo" component={Promo} durationInFrames={TOTAL} fps={30} width={1920} height={1080} />
		<Composition id="ModelTest" component={ModelTest} durationInFrames={1} fps={30} width={1280} height={720} defaultProps={{view: 0, lid: 110}} />
		<Composition id="Cover" component={Cover} durationInFrames={300} fps={30} width={1280} height={640} />
	</>
);
