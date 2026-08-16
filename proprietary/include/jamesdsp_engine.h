#ifndef JAMESDSP_ENGINE_H
#define JAMESDSP_ENGINE_H

#include <stdint.h>
#include "jdsp/jdsp_header.h"
#include "essential.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	unsigned long long initializeForFirst;
	int32_t engineImpulseResponseHash;
	int32_t hashSlot[4];
	int mEnable;
	JamesDSPLib jdsp;
	float mSamplingRate;
	int formatFloatModeInt32Mode;
	char *stringEq;
	float *tempImpulseIncoming;
	float drcAtkRel[4];
	float boostingCon;
	int bbMaxGain;
	int numTime2Send, samplesInc, stringIndex;
	int16_t impChannels;
	int32_t impulseLengthActual, convolverNeedRefresh;
} EffectDSPMain;

void EffectDSPMainConstructor(EffectDSPMain *dspmain);
void EffectDSPMainDestructor(EffectDSPMain *dspmain);
int32_t configure(EffectDSPMain *dspmain, void *pCmdData, effect_buffer_access_e *mAccessMode);
int32_t EffectDSPMainCommand(EffectDSPMain *dspmain, uint32_t cmdCode, uint32_t cmdSize, void *pCmdData, uint32_t *replySize, void *pReplyData);
int32_t EffectDSPMainProcess(EffectDSPMain *dspmain, audio_buffer_t *in, audio_buffer_t *out);

#ifdef __cplusplus
}
#endif

#endif
