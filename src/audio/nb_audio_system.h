// nb - the audio system the game gets: SDL when the machine has a playback device, silence when it does
// not.
//
// Without a default playback endpoint (a USB headset switched off, say) SDL_OpenAudioDeviceStream fails,
// the SDK's AudioSystem::RegisterClient returns an error to the guest, and the game dereferences a null
// pointer a few hundred milliseconds into the boot. The SDK's NopAudioSystem is not a way out: its
// CreateDriver returns NOT_IMPLEMENTED, which is the same failure. So the fallback here is a driver that
// accepts frames and paces the client the way a real device would, only without playing anything.

#pragma once

#include <chrono>
#include <memory>

#include <rex/audio/audio_driver.h>
#include <rex/audio/sdl/sdl_audio_system.h>

namespace nb::audio {

// X_STATUS_SUCCESS and friends are macros that name the unqualified type.
using rex::X_STATUS;

// Consumes submitted frames at the rate the guest expects: one 256-sample frame per channel at 48 kHz,
// releasing the client semaphore so the game's audio thread keeps its cadence instead of spinning.
class SilentAudioDriver : public rex::audio::AudioDriver {
 public:
  SilentAudioDriver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~SilentAudioDriver() override;
  void SubmitFrame(uint32_t samples_ptr) override;

 private:
  // 256 samples a channel at 48 kHz, the rate the game submits at.
  static constexpr std::chrono::nanoseconds kFrameDuration{256 * 1000000000ull / 48000};

  // Consumes queued frames at that rate, the way a device's callback thread would. Releasing from
  // SubmitFrame instead would either block the guest thread that called it or let the game run away.
  void Consume();

  rex::thread::Semaphore* semaphore_ = nullptr;
  std::atomic<uint32_t> queued_{0};
  std::atomic<bool> running_{true};
  std::thread worker_;
};

class NbAudioSystem : public rex::audio::sdl::SDLAudioSystem {
 public:
  using rex::audio::sdl::SDLAudioSystem::SDLAudioSystem;

  static std::unique_ptr<rex::audio::AudioSystem> Create(
      rex::runtime::FunctionDispatcher* function_dispatcher);

  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        rex::audio::AudioDriver** out_driver) override;
  void DestroyDriver(rex::audio::AudioDriver* driver) override;
};

}  // namespace nb::audio
