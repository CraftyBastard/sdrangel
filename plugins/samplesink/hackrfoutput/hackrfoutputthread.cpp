///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2017 Edouard Griffiths, F4EXB                                   //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
// (at your option) any later version.                                           //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#include "hackrfoutputthread.h"

#include <stdio.h>
#include <errno.h>

#include "dsp/samplesourcefifo.h"
#include "hackrf/devicehackrf.h"

HackRFOutputThread::HackRFOutputThread(hackrf_device* dev, SampleSourceFifo* sampleFifo, QObject* parent) :
	QThread(parent),
	m_running(false),
	m_dev(dev),
	m_sampleFifo(sampleFifo),
	m_log2Interp(0),
    m_fcPos(2),
    m_markTxOnCount(-1),
    m_markTxOffCount(-1)
{
    std::fill(m_buf, m_buf + 2*HACKRF_BLOCKSIZE, 0);

    QObject::connect(
        this,
        &HackRFOutputThread::signalTxEnable,
        this,
        &HackRFOutputThread::enableTx,
        Qt::QueuedConnection
    );
}

HackRFOutputThread::~HackRFOutputThread()
{
	stopWork();
}

void HackRFOutputThread::startWork()
{
	m_startWaitMutex.lock();
	start();
	while(!m_running)
		m_startWaiter.wait(&m_startWaitMutex, 100);
	m_startWaitMutex.unlock();
}

void HackRFOutputThread::stopWork()
{
    if (!m_running) return;
	qDebug("HackRFOutputThread::stopWork");
	m_running = false;
	wait();
}

void HackRFOutputThread::setLog2Interpolation(unsigned int log2Interp)
{
	m_log2Interp = log2Interp;
}

void HackRFOutputThread::setFcPos(int fcPos)
{
	m_fcPos = fcPos;
}

void HackRFOutputThread::markTx(bool enable)
{
    int nbChunks = (0.8192f * m_sampleRate) / 131072.0f;
    qDebug("HackRFOutputThread::markTx: m_sampleRate: %d nbChumks: %d enable: %s",
        m_sampleRate, nbChunks, enable ? "on" : "off");

    if (enable) {
        m_markTxOnCount = nbChunks;
    } else {
        m_markTxOffCount = nbChunks + 2;
    }
}

void HackRFOutputThread::run()
{
	hackrf_error rc;

    m_running = true;
    m_startWaiter.wakeAll();


    if (hackrf_is_streaming(m_dev) == HACKRF_TRUE)
    {
        qDebug("HackRFInputThread::run: HackRF is streaming already");
    }
    else
    {
        qDebug("HackRFInputThread::run: HackRF is not streaming");

        rc = (hackrf_error) hackrf_start_tx(m_dev, tx_callback, this);

        if (rc == HACKRF_SUCCESS)
        {
            qDebug("HackRFOutputThread::run: started HackRF Tx");
        }
        else
        {
            qDebug("HackRFOutputThread::run: failed to start HackRF Tx: %s", hackrf_error_name(rc));
        }
    }

    while ((m_running) && (hackrf_is_streaming(m_dev) == HACKRF_TRUE))
    {
        usleep(200000);
    }

    if (hackrf_is_streaming(m_dev) == HACKRF_TRUE)
    {
        rc = (hackrf_error) hackrf_stop_tx(m_dev);

        if (rc == HACKRF_SUCCESS)
        {
            qDebug("HackRFOutputThread::run: stopped HackRF Tx");
        }
        else
        {
            qDebug("HackRFOutputThread::run: failed to stop HackRF Tx: %s", hackrf_error_name(rc));
        }
    }

    DeviceHackRF::setMixers(m_dev, true); // restore mixer state
	m_running = false;
}

//  Interpolate according to specified log2 (ex: log2=4 => interp=16)
void HackRFOutputThread::callback(qint8* buf, qint32 len)
{
    SampleVector& data = m_sampleFifo->getData();
    unsigned int iPart1Begin, iPart1End, iPart2Begin, iPart2End;
    m_sampleFifo->read(len/(2*(1<<m_log2Interp)), iPart1Begin, iPart1End, iPart2Begin, iPart2End);

    if (iPart1Begin != iPart1End) {
        callbackPart(buf, data, iPart1Begin, iPart1End);
    }

    unsigned int shift = (iPart1End - iPart1Begin)*(1<<m_log2Interp);

    if (iPart2Begin != iPart2End) {
        callbackPart(buf + 2*shift, data, iPart2Begin, iPart2End);
    }

    if (m_markTxOnCount >= 0) {
        m_markTxOnCount--;
    }

    if (m_markTxOffCount >= 0) {
        m_markTxOffCount--;
    }

    if (m_markTxOnCount == 0) {
        emit signalTxEnable(true);
    }

    if (m_markTxOffCount == 0) {
        emit signalTxEnable(false);
    }
}

void HackRFOutputThread::callbackPart(qint8* buf, SampleVector& data, unsigned int iBegin, unsigned int iEnd)
{
    SampleVector::iterator beginRead = data.begin() + iBegin;
    int len = 2*(iEnd - iBegin)*(1<<m_log2Interp);

	if (m_log2Interp == 0)
	{
	    m_interpolators.interpolate1(&beginRead, buf, len);
	}
	else
	{
		if (m_fcPos == 0) // Infra
		{
            switch (m_log2Interp)
            {
            case 1:
                m_interpolators.interpolate2_inf(&beginRead, buf, len);
                break;
            case 2:
                m_interpolators.interpolate4_inf(&beginRead, buf, len);
                break;
            case 3:
                m_interpolators.interpolate8_inf(&beginRead, buf, len);
                break;
            case 4:
                m_interpolators.interpolate16_inf(&beginRead, buf, len);
                break;
            case 5:
                m_interpolators.interpolate32_inf(&beginRead, buf, len);
                break;
            case 6:
                m_interpolators.interpolate64_inf(&beginRead, buf, len);
                break;
            default:
                break;
            }
        }
        else if (m_fcPos == 1) // Supra
        {
            switch (m_log2Interp)
            {
            case 1:
                m_interpolators.interpolate2_sup(&beginRead, buf, len);
                break;
            case 2:
                m_interpolators.interpolate4_sup(&beginRead, buf, len);
                break;
            case 3:
                m_interpolators.interpolate8_sup(&beginRead, buf, len);
                break;
            case 4:
                m_interpolators.interpolate16_sup(&beginRead, buf, len);
                break;
            case 5:
                m_interpolators.interpolate32_sup(&beginRead, buf, len);
                break;
            case 6:
                m_interpolators.interpolate64_sup(&beginRead, buf, len);
                break;
            default:
                break;
            }
        }
        else if (m_fcPos == 2) // Center
        {
            switch (m_log2Interp)
            {
            case 1:
                m_interpolators.interpolate2_cen(&beginRead, buf, len);
                break;
            case 2:
                m_interpolators.interpolate4_cen(&beginRead, buf, len);
                break;
            case 3:
                m_interpolators.interpolate8_cen(&beginRead, buf, len);
                break;
            case 4:
                m_interpolators.interpolate16_cen(&beginRead, buf, len);
                break;
            case 5:
                m_interpolators.interpolate32_cen(&beginRead, buf, len);
                break;
            case 6:
                m_interpolators.interpolate64_cen(&beginRead, buf, len);
                break;
            default:
                break;
            }
        }
	}
}

int HackRFOutputThread::tx_callback(hackrf_transfer* transfer)
{
    HackRFOutputThread *thread = (HackRFOutputThread *) transfer->tx_ctx;
    qint32 bytes_to_write = transfer->valid_length;
	thread->callback((qint8 *) transfer->buffer, bytes_to_write);
    return 0;
}

void HackRFOutputThread::enableTx(bool on)
{
    if (m_dev)
    {
        // qDebug("HackRFOutputThread::enableTx: %s", on ? "on" : "off");
        DeviceHackRF::setMixers(m_dev, on);
    }
}
