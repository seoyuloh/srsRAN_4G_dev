#ifndef SRSRAN_TUNER_H
#define SRSRAN_TUNER_H

#include "srsran/config.h"
#include "srsran/phy/common/timestamp.h"
#include "srsran/srslog/logger.h"
#include <atomic>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

// C++ Class Definition
class srsran_channel_tuner_t {
public:
  std::atomic<float> tuner_attenuation;
  std::unique_ptr<std::thread> tuner_monitor_thread;
  srslog::basic_logger& logger;
  std::ifstream sock;
  std::string domain_socket_name;

  // Constructor
  srsran_channel_tuner_t(srslog::basic_logger& logger_ref,
                         const std::string&    tuner_name         = "Tuner",
                         const std::string&    domain_socket_name = "/tmp/uetuner.sock",
                         float                 attenuation        = 1.0f) :
    tuner_attenuation(attenuation), logger(logger_ref), domain_socket_name(domain_socket_name), sock(domain_socket_name)
  {
    std::cout<<"tuner: "<<tuner_name<<"\n";
    // Initialize the thread in the constructor
    tuner_monitor_thread = std::make_unique<std::thread>([this]() {
      float new_gain;
      do {
        sock >> new_gain;
        if (sock) {
          tuner_attenuation.store(new_gain, std::memory_order_relaxed);
          logger.info("Attenuation changed to %f", new_gain);
        } else {
          sock.clear();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      } while (new_gain > 0);
      logger.info("Tuner monitor thread stopping.");
    });
  }
};


// C-Compatible API
#ifdef __cplusplus
extern "C" {
#endif

  SRSRAN_API void srsran_channel_tuner_execute(srsran_channel_tuner_t* q,
                                               const cf_t* in,
                                               cf_t* out,
                                               uint32_t nsamples);

#ifdef __cplusplus
}
#endif

#endif // SRSRAN_TUNER_H
