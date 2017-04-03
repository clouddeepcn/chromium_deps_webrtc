# Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
#
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file in the root of the source
# tree. An additional intellectual property rights grant can be found
# in the file PATENTS.  All contributing project authors may
# be found in the AUTHORS file in the root of the source tree.

import array
import logging
import sys

try:
  import numpy as np
except ImportError:
  logging.critical('Cannot import the third-party Python package numpy')
  sys.exit(1)

try:
  import pydub
  import pydub.generators
except ImportError:
  logging.critical('Cannot import the third-party Python package pydub')
  sys.exit(1)

try:
  import scipy.signal
except ImportError:
  logging.critical('Cannot import the third-party Python package scipy')
  sys.exit(1)


class SignalProcessingException(Exception):
  pass


class SignalProcessingUtils(object):

  def __init__(self):
    pass

  @classmethod
  def load_wav(cls, filepath, channels=1):
    """Load wav file.

    Return:
      AudioSegment instance.
    """
    return pydub.AudioSegment.from_file(
        filepath, format='wav', channels=channels)

  @classmethod
  def save_wav(cls, output_filepath, signal):
    """Save wav file.

    Args:
      output_filepath: string, output file path.
      signal: AudioSegment instance.
    """
    return signal.export(output_filepath, format='wav')

  @classmethod
  def count_samples(cls, signal):
    """Number of samples per channel.

    Args:
      signal: AudioSegment instance.
    """
    number_of_samples = len(signal.get_array_of_samples())
    assert signal.channels > 0
    assert number_of_samples % signal.channels == 0
    return number_of_samples / signal.channels

  @classmethod
  def generate_white_noise(cls, signal):
    """Generate white noise.

    Generate white noise with the same duration and in the same format as a
    given signal.

    Args:
      signal: AudioSegment instance.

    Return:
      AudioSegment instance.
    """
    generator = pydub.generators.WhiteNoise(
        sample_rate=signal.frame_rate,
        bit_depth=signal.sample_width * 8)
    return generator.to_audio_segment(
        duration=len(signal),
        volume=0.0)

  @classmethod
  def apply_impulse_response(cls, signal, impulse_response):
    """Apply an impulse response to a signal.
    """
    # Get samples.
    assert signal.channels == 1, (
        'multiple-channel recordings not supported')
    samples = signal.get_array_of_samples()

    # Convolve.
    logging.info('applying %d order impulse response to a signal lasting %d ms',
                 len(impulse_response), len(signal))
    convolved_samples = scipy.signal.fftconvolve(
        in1=samples,
        in2=impulse_response,
        mode='full').astype(np.int16)
    logging.info('convolution computed')

    # Cast.
    convolved_samples = array.array(signal.array_type, convolved_samples)

    # Verify.
    logging.debug('signal length: %d samples', len(samples))
    logging.debug('convolved signal length: %d samples', len(convolved_samples))
    assert len(convolved_samples) > len(samples)

    # Generate convolved signal AudioSegment instance.
    convolved_signal = pydub.AudioSegment(
        data=convolved_samples,
        metadata={
            'sample_width': signal.sample_width,
            'frame_rate': signal.frame_rate,
            'frame_width': signal.frame_width,
            'channels': signal.channels,
        })
    assert len(convolved_signal) > len(signal)

    return convolved_signal

  @classmethod
  def normalize(cls, signal):
    return signal.apply_gain(-signal.max_dBFS)

  @classmethod
  def copy(cls, signal):
    return pydub.AudioSegment(
        data=signal.get_array_of_samples(),
        metadata={
            'sample_width': signal.sample_width,
            'frame_rate': signal.frame_rate,
            'frame_width': signal.frame_width,
            'channels': signal.channels,
        })

  @classmethod
  def mix_signals(cls, signal, noise, target_snr=0.0,
                  bln_pad_shortest=False):
    """Mix two signals with a target SNR.

    Mix two signals up to a desired SNR by scaling noise (noise).
    If the target SNR is +/- infinite, a copy of signal/noise is returned.

    Args:
      signal: AudioSegment instance (signal).
      noise: AudioSegment instance (noise).
      target_snr: float, numpy.Inf or -numpy.Inf (dB).
      bln_pad_shortest: if True, it pads the shortest signal with silence at the
                        end.
    """
    # Handle infinite target SNR.
    if target_snr == -np.Inf:
      # Return a copy of noise.
      logging.warning('SNR = -Inf, returning noise')
      return cls.copy(noise)
    elif target_snr == np.Inf:
      # Return a copy of signal.
      logging.warning('SNR = +Inf, returning signal')
      return cls.copy(signal)

    # Check signal and noise power.
    signal_power = float(signal.dBFS)
    noise_power = float(noise.dBFS)
    if signal_power == -np.Inf:
      logging.error('signal has -Inf power, cannot mix')
      raise SignalProcessingException('cannot mix a signal with -Inf power')
    if noise_power == -np.Inf:
      logging.error('noise has -Inf power, cannot mix')
      raise SignalProcessingException('cannot mix a signal with -Inf power')

    # Pad signal (if necessary). If noise is the shortest, the AudioSegment
    # overlay() method implictly pads noise. Hence, the only case to handle
    # is signal shorter than noise and bln_pad_shortest True.
    if bln_pad_shortest:
      signal_duration = len(signal)
      noise_duration = len(noise)
      logging.warning('mix signals with padding')
      logging.warning('  signal: %d ms', signal_duration)
      logging.warning('  noise: %d ms', noise_duration)
      padding_duration = noise_duration - signal_duration
      if padding_duration > 0:  # That is signal_duration < noise_duration.
        logging.debug('  padding: %d ms', padding_duration)
        padding = pydub.AudioSegment.silent(
            duration=padding_duration,
            frame_rate=signal.frame_rate)
        logging.debug('  signal (pre): %d ms', len(signal))
        signal = signal + padding
        logging.debug('  signal (post): %d ms', len(signal))

        # Update power.
        signal_power = float(signal.dBFS)

    # Mix signals using the target SNR.
    gain_db = signal_power - noise_power - target_snr
    return cls.normalize(signal.overlay(noise.apply_gain(gain_db)))
