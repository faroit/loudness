/*
 * Copyright (C) 2014 Dominic Ward <contactdominicward@gmail.com>
 *
 * This file is part of Loudness
 *
 * Loudness is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Loudness is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Loudness.  If not, see <http://www.gnu.org/licenses/>. 
 */

#include "PowerSpectrum.h"

namespace loudness{

    PowerSpectrum::PowerSpectrum(const RealVec& bandFreqsHz,
                                 const vector<int>& windowSizes, 
                                 bool sampleSpectrumUniformly)
        :   Module("PowerSpectrum"),
            bandFreqsHz_(bandFreqsHz),
            windowSizes_(windowSizes),
            sampleSpectrumUniformly_(sampleSpectrumUniformly),
            normalisation_(AVERAGE_POWER),
            referenceValue_(2e-5)
    {}

    PowerSpectrum::~PowerSpectrum()
    {}

    bool PowerSpectrum::initializeInternal(const SignalBank &input)
    {
        
        ffts_.clear();

        //number of windows
        int nWindows = (int)windowSizes_.size();
        LOUDNESS_ASSERT(input.getNChannels() == nWindows,
                name_ << ": Number of channels do not match number of windows");
        LOUDNESS_ASSERT((int)bandFreqsHz_.size() == (nWindows + 1),
                name_ << ": Number of frequency bands should equal number of input channels + 1.");
        LOUDNESS_ASSERT(!anyAscendingValues(windowSizes_),
                    name_ << ": Window lengths must be in descending order.");

        //work out FFT configuration (constrain to power of 2)
        int largestWindowSize = input.getNSamples();
        vector<int> fftSize(nWindows, nextPowerOfTwo(largestWindowSize));
        if(sampleSpectrumUniformly_)
        {
            ffts_.push_back(unique_ptr<FFT> (new FFT(fftSize[0]))); 
            ffts_[0] -> initialize();
        }
        else
        {
            for(int w=0; w<nWindows; w++)
            {
                fftSize[w] = nextPowerOfTwo(windowSizes_[w]);
                ffts_.push_back(unique_ptr<FFT> (new FFT(fftSize[w]))); 
                ffts_[w] -> initialize();
            }
        }

        //desired bins indices (lo and hi) per band
        bandBinIndices_.resize(nWindows);
        normFactor_.resize(nWindows);
        int fs = input.getFs();
        int nBins = 0;
        for(int i=0; i<nWindows; i++)
        {
            //bin indices to use for compiled spectrum
            bandBinIndices_[i].resize(2);
            //These are NOT the nearest components but satisfies f_k in [f_lo, f_hi)
            bandBinIndices_[i][0] = ceil(bandFreqsHz_[i]*fftSize[i]/fs);
            // use < bandBinIndices_[i][1] to exclude upper bin
            bandBinIndices_[i][1] = ceil(bandFreqsHz_[i+1]*fftSize[i]/fs);
            LOUDNESS_ASSERT(bandBinIndices_[i][1]>0, 
                    name_ << ": No components found in band number " << i);

            //exclude DC and Nyquist if found
            int nyqIdx = (fftSize[i]/2) + (fftSize[i]%2);
            if(bandBinIndices_[i][0]==0)
            {
                LOUDNESS_WARNING(name_ << ": DC found...excluding.");
                bandBinIndices_[i][0] = 1;
            }
            if((bandBinIndices_[i][1]-1) >= nyqIdx)
            {
                LOUDNESS_WARNING(name_ << 
                        ": Bin is >= nyquist...excluding.");
                bandBinIndices_[i][1] = nyqIdx;
            }

            nBins += bandBinIndices_[i][1]-bandBinIndices_[i][0];

            //Power spectrum normalisation
            Real refSquared = referenceValue_ * referenceValue_;
            switch (normalisation_)
            {
                case NONE:
                    normFactor_[i] = 1.0 / refSquared;
                    break;
                case ENERGY:
                    normFactor_[i] = 2.0/(fftSize[i] * refSquared);
                    break;
                case AVERAGE_POWER:
                    normFactor_[i] = 2.0/(fftSize[i] * windowSizes_[i] * refSquared);
                    break;
                default:
                    normFactor_[i] = 2.0/(fftSize[i] * refSquared);
            }

            LOUDNESS_DEBUG(name_ << ": Normalisation factor : " << normFactor_[i]);
        }

        //total number of bins in the output spectrum
        LOUDNESS_DEBUG(name_ 
                << ": Total number of bins comprising the output spectrum: " << nBins);

        //initialize the output SignalBank
        output_.initialize(input.getNEars(), nBins, 1, fs);
        output_.setFrameRate(input.getFrameRate());

        //output frequencies in Hz
        int j = 0, k = 0;
        for(int i=0; i<nWindows; i++)
        {
            j = bandBinIndices_[i][0];
            while(j < bandBinIndices_[i][1])
                output_.setCentreFreq(k++, (j++)*fs/(Real)fftSize[i]);

            LOUDNESS_DEBUG(name_ 
                    << ": Included freq Hz (band low): " 
                    << fs * bandBinIndices_[i][0] / float(fftSize[i]) 
                    << ": Included freq Hz (band high): " 
                    << fs * (bandBinIndices_[i][1] - 1) / float(fftSize[i])); 
        }

        return 1;
    }

    void PowerSpectrum::processInternal(const SignalBank &input)
    {
        int fftIdx = 0;
        int nWindows = windowSizes_.size();
        for (int ear = 0; ear < input.getNEars(); ear++)
        {
            //get a single sample pointer for moving through channels
            Real* outputSignal = output_.getSingleSampleWritePointer(ear,0);

            for(int chn=0; chn<nWindows; chn++)
            {
                if(!sampleSpectrumUniformly_)
                    fftIdx = chn;

                //Do the FFT
                ffts_[fftIdx] -> process(input.getSignalReadPointer(ear, chn, 0), windowSizes_[chn]);

                //Extract components from band and compute powers
                Real re, im;
                int bin = bandBinIndices_[chn][0];
                while(bin < bandBinIndices_[chn][1])
                {
                    re = ffts_[fftIdx] -> getReal(bin);
                    im = ffts_[fftIdx] -> getImag(bin++);
                    *outputSignal++ = normFactor_[chn] * (re*re + im*im);
                }
            }
        }
    }

    void PowerSpectrum::resetInternal()
    {}

    void PowerSpectrum::setNormalisation(const Normalisation normalisation)
    {
        normalisation_ = normalisation;
    }

    void PowerSpectrum::setReferenceValue(Real referenceValue)
    {
        referenceValue_ = referenceValue;
    }
}
