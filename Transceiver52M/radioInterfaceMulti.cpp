/*
 * Multi-carrier radio interface
 *
 * Copyright (C) 2016 Ettus Research LLC 
 *
 * Author: Tom Tsou <tom.tsou@ettus.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * See the COPYING file in the main directory for details.
 */

#include <radioInterface.h>

#include "Resampler.h"

extern "C" {
#include "Logging.h"
#include "common/convert.h"
}

/* Resampling parameters for 64 MHz clocking */
#define RESAMP_INRATE			65
#define RESAMP_OUTRATE			(96 / 2)

/* Universal resampling parameters */
#define NUMCHUNKS				24

#define MCHANS					4

RadioInterfaceMulti::RadioInterfaceMulti(RadioDevice *radio, size_t tx_sps,
					 size_t rx_sps, size_t chans)
	: RadioInterface(radio, tx_sps, rx_sps, chans),
	  outerSendBuffer(NULL), outerRecvBuffer(NULL),
	  dnsampler(NULL), upsampler(NULL), channelizer(NULL), synthesis(NULL)
{
}

RadioInterfaceMulti::~RadioInterfaceMulti()
{
	close();
}

void RadioInterfaceMulti::close()
{
	delete outerSendBuffer;
	delete outerRecvBuffer;
	delete dnsampler;
	delete upsampler;
	delete channelizer;
	delete synthesis;

	outerSendBuffer = NULL;
	outerRecvBuffer = NULL;
	dnsampler = NULL;
	upsampler = NULL;
	channelizer = NULL;
	synthesis = NULL;

	mReceiveFIFO.resize(0);
	powerScaling.resize(0);
	history.resize(0);
	active.resize(0);

	RadioInterface::close();
}

static int getLogicalChan(size_t pchan, size_t chans)
{
	switch (chans) {
	case 1:
		if (pchan == 0)
			return 0;
		else
			return -1;
		break;
	case 2:
		if (pchan == 0)
			return 0;
		if (pchan == 3)
			return 1;
		else
			return -1;
		break;
	case 3:
		if (pchan == 1)
			return 0;
		if (pchan == 0)
			return 1;
		if (pchan == 3)
			return 2;
		else
			return -1;
		break;
	default:
		break;
	};

	return -1;
}

static int getFreqShift(size_t chans)
{
	switch (chans) {
	case 1:
		return 0;
	case 2:
		return 0;
	case 3:
		return 1;
	default:
		break;
	};

	return -1;
}

/* Initialize I/O specific objects */
bool RadioInterfaceMulti::init(int type)
{
	float cutoff = 1.0f;
	size_t inchunk = 0, outchunk = 0;

	if (mChans > MCHANS - 1) {
		LOGP(DDSP, LOGL_FATAL,
                    "Invalid channel configuration %zu\n", mChans);
		return false;
	}

	close();

	sendBuffer.resize(mChans);
	recvBuffer.resize(mChans);
	convertSendBuffer.resize(1);
	convertRecvBuffer.resize(1);

	mReceiveFIFO.resize(mChans);
	powerScaling.resize(mChans);
	history.resize(mChans);
	active.resize(MCHANS, false);

	inchunk = RESAMP_INRATE * 4;
	outchunk = RESAMP_OUTRATE * 4;

	if (inchunk  * NUMCHUNKS < 625 * 2) {
		LOGP(DDSP, LOGL_FATAL,
		     "Invalid inner chunk size %zu\n", inchunk);
		return false;
	}

	dnsampler = new Resampler(RESAMP_INRATE, RESAMP_OUTRATE);
	if (!dnsampler->init(1.0)) {
		LOGP(DDSP, LOGL_FATAL, "Rx resampler failed to initialize\n");
		return false;
	}

	upsampler = new Resampler(RESAMP_OUTRATE, RESAMP_INRATE);
	if (!upsampler->init(cutoff)) {
		LOGP(DDSP, LOGL_FATAL, "Tx resampler failed to initialize\n");
		return false;
	}

	channelizer = new Channelizer(MCHANS, outchunk);
	if (!channelizer->init()) {
		LOGP(DDSP, LOGL_FATAL, "Rx channelizer failed to initialize\n");
		return false;
	}

	synthesis = new Synthesis(MCHANS, outchunk);
	if (!synthesis->init()) {
		LOGP(DDSP, LOGL_FATAL,
		     "Tx synthesis filter failed to initialize\n");
		return false;
	}

	/*
	 * Allocate high and low rate buffers. The high rate receive
	 * buffer and low rate transmit vectors feed into the resampler
	 * and requires headroom equivalent to the filter length. Low
	 * rate buffers are allocated in the main radio interface code.
	 */
	for (size_t i = 0; i < mChans; i++) {
		sendBuffer[i] = new RadioBuffer(NUMCHUNKS, inchunk,
					        upsampler->len(), true);
		recvBuffer[i] = new RadioBuffer(NUMCHUNKS, inchunk,
		                                0, false);
		history[i] = new signalVector(dnsampler->len());

		synthesis->resetBuffer(i);
	}

	outerSendBuffer = new signalVector(synthesis->outputLen());
	outerRecvBuffer = new signalVector(channelizer->inputLen());

	convertSendBuffer[0] = new short[2 * synthesis->outputLen()];
	convertRecvBuffer[0] = new short[2 * channelizer->inputLen()];

	/* Configure channels */
	switch (mChans) {
	case 1:
		active[0] = true;
		break;
	case 2:
		active[0] = true;
		active[3] = true;
		break;
	case 3:
		active[0] = true;
		active[1] = true;
		active[3] = true;
		break;
	default:
		LOGP(DDSP, LOGL_FATAL, "Unsupported channel combination\n");
		return false;
	}

	return true;
}

/* Receive a timestamped chunk from the device */
void RadioInterfaceMulti::pullBuffer()
{
	bool local_underrun;
	size_t num;
	float *buf;

	if (recvBuffer[0]->getFreeSegments() <= 0)
		return;

	/* Outer buffer access size is fixed */
	num = mRadio->readSamples(convertRecvBuffer,
				  outerRecvBuffer->size(),
				  &overrun,
				  readTimestamp,
				  &local_underrun);
	if (num != channelizer->inputLen()) {
		LOGP(DDSP, LOGL_FATAL,
		     "Receive error: Expected %zu samples, but got %zu\n",
		     channelizer->inputLen(), num);
		return;
	}

	convert_short_float((float *) outerRecvBuffer->begin(),
			    convertRecvBuffer[0], 2 * outerRecvBuffer->size());

	underrun |= local_underrun;
	readTimestamp += num;

	channelizer->rotate((float *) outerRecvBuffer->begin(),
			    outerRecvBuffer->size());

	for (size_t pchan = 0; pchan < MCHANS; pchan++) {
		if (!active[pchan])
			continue;

		int lchan = getLogicalChan(pchan, mChans);
		if (lchan < 0) {
			LOGP(DDSP, LOGL_FATAL,
			     "Invalid logical channel %zu\n", pchan);
			continue;
		}

		/*
		 * Update history by writing into the head portion of the
		 * channelizer output buffer. For this to work, filter length of
		 * the polyphase channelizer partition filter should be equal to
		 * or larger than the resampling filter.
		 */
		buf = channelizer->outputBuffer(pchan);
		size_t cLen = channelizer->outputLen();
		size_t hLen = dnsampler->len();
		size_t hSize = 2 * hLen * sizeof(float);

		memcpy(&buf[2 * -hLen], history[lchan]->begin(), hSize);
		memcpy(history[lchan]->begin(), &buf[2 * (cLen - hLen)], hSize);

		float *wr_segment = recvBuffer[lchan]->getWriteSegment();

		/* Write to the end of the inner receive buffer */
		if (!dnsampler->rotate(channelizer->outputBuffer(pchan),
				       channelizer->outputLen(),
				       wr_segment,
				       recvBuffer[lchan]->getSegmentLen())) {
			LOGP(DDSP, LOGL_FATAL,
			     "Sample rate upsampling failed\n");
		}
	}
}

/* Send a timestamped chunk to the device */
bool RadioInterfaceMulti::pushBuffer()
{
	if (sendBuffer[0]->getAvailSegments() <= 0)
		return false;

	for (size_t pchan = 0; pchan < MCHANS; pchan++) {
		if (!active[pchan]) {
			synthesis->resetBuffer(pchan);
			continue;
		}

		int lchan = getLogicalChan(pchan, mChans);
		if (lchan < 0) {
			LOGP(DDSP, LOGL_FATAL,
			     "Invalid logical channel %zu\n", pchan);
			continue;
		}

		if (!upsampler->rotate(sendBuffer[lchan]->getReadSegment(),
				       sendBuffer[lchan]->getSegmentLen(),
				       synthesis->inputBuffer(pchan),
				       synthesis->inputLen())) {
			LOGP(DDSP, LOGL_FATAL,
			     "Sample rate downsampling failed\n");
		}
	}

	synthesis->rotate((float *) outerSendBuffer->begin(),
			  outerSendBuffer->size());

	convert_float_short(convertSendBuffer[0],
			    (float *) outerSendBuffer->begin(),
			    1.0 / (float) mChans, 2 * outerSendBuffer->size());

	size_t num = mRadio->writeSamples(convertSendBuffer,
					  outerSendBuffer->size(),
					  &underrun,
					  writeTimestamp);
	if (num != outerSendBuffer->size()) {
		LOGP(DDSP, LOGL_FATAL, "Transmit error %zu\n", num);;
	}

	writeTimestamp += num;

	return true;
}

/* Frequency comparison limit */
#define FREQ_DELTA_LIMIT		10.0

static bool fltcmp(double a, double b)
{
	return fabs(a - b) < FREQ_DELTA_LIMIT ? true : false;
}

bool RadioInterfaceMulti::tuneTx(double freq, size_t chan)
{
  if (chan >= mChans)
    return false;

  double shift = (double) getFreqShift(mChans);

  if (!chan)
    return mRadio->setTxFreq(freq + shift * MCBTS_SPACING);

  double center = mRadio->getTxFreq();
  if (!fltcmp(freq, center + (double) (chan - shift) * MCBTS_SPACING)) {
    LOGP(DTRX, LOGL_NOTICE,
         "Channel %zu RF frequency offset is %f MHz\n", chan, freq / 1e6);
  }

  return true;
}

bool RadioInterfaceMulti::tuneRx(double freq, size_t chan)
{
  if (chan >= mChans)
    return false;

  double shift = (double) getFreqShift(mChans);

  if (!chan)
    return mRadio->setRxFreq(freq + shift * MCBTS_SPACING);

  double center = mRadio->getRxFreq();
  if (!fltcmp(freq, center + (double) (chan - shift) * MCBTS_SPACING)) {
    LOGP(DTRX, LOGL_NOTICE,
         "Channel %zu RF frequency offset is %f MHz\n", chan, freq / 1e6);
  }

  return true;
}

double RadioInterfaceMulti::setRxGain(double db, size_t chan)
{
  if (!chan)
    return mRadio->setRxGain(db);
  else
    return mRadio->getRxGain();
}
