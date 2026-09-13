// See nb_audio_system.h.

#include "nb_audio_system.h"

#include <thread>

#include <rex/logging.h>

namespace nb::audio {

SilentAudioDriver::SilentAudioDriver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : rex::audio::AudioDriver(memory), semaphore_(semaphore), worker_([this]() { Consume(); }) {}

SilentAudioDriver::~SilentAudioDriver() {
  running_ = false;
  if (worker_.joinable()) {
    worker_.join();
  }
}

void SilentAudioDriver::Consume() {
  auto next = std::chrono::steady_clock::now();
  while (running_) {
    next += kFrameDuration;
    std::this_thread::sleep_until(next);
    uint32_t queued = queued_.load(std::memory_order_relaxed);
    while (queued > 0 && !queued_.compare_exchange_weak(queued, queued - 1)) {
    }
    if (queued > 0 && semaphore_) {
      semaphore_->Release(1, nullptr);
    }
  }
}

void SilentAudioDriver::SubmitFrame(uint32_t /*samples_ptr*/) {
  queued_.fetch_add(1, std::memory_order_relaxed);
}

std::unique_ptr<rex::audio::AudioSystem> NbAudioSystem::Create(
    rex::runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<NbAudioSystem>(function_dispatcher);
}

X_STATUS NbAudioSystem::CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                                     rex::audio::AudioDriver** out_driver) {
  const X_STATUS status = SDLAudioSystem::CreateDriver(index, semaphore, out_driver);
  if (XSUCCEEDED(status)) {
    return status;
  }
  static bool warned = false;
  if (!warned) {
    warned = true;
    REXLOG_WARN("nb: no audio playback device; the game runs silent (plug one in and restart for sound)");
  }
  *out_driver = new SilentAudioDriver(memory_, semaphore);
  return X_STATUS_SUCCESS;
}

void NbAudioSystem::DestroyDriver(rex::audio::AudioDriver* driver) {
  if (auto* silent = dynamic_cast<SilentAudioDriver*>(driver)) {
    delete silent;
    return;
  }
  SDLAudioSystem::DestroyDriver(driver);
}

}  // namespace nb::audio
