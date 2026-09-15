#pragma once

#include "research/pvr_ta_observation.h"

namespace pvr_ta_test
{

class PvrSubscription
{
public:
	explicit PvrSubscription(research::PvrTaObservationSubscription id)
		: id(id) {}
	~PvrSubscription() { research::unsubscribePvrTaObservations(id); }

	PvrSubscription(const PvrSubscription&) = delete;
	PvrSubscription& operator=(const PvrSubscription&) = delete;

private:
	research::PvrTaObservationSubscription id;
};

inline research::PvrTaRenderSelectionTranscript selectionTranscript()
{
	research::PvrTaRenderSelectionTranscript transcript;
	transcript.initialized = true;
	transcript.regionBase = 0x00200000;
	transcript.fpuParamCfg = 0;
	transcript.record(0x00200010, 0);
	transcript.record(0x00200000, 0x80000000);
	transcript.record(0x00200004, 0x00300000);
	transcript.record(0x00300000, 0x00100000);
	return transcript;
}

} // namespace pvr_ta_test
