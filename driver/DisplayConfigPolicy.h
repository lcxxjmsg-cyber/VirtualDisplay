#pragma once

namespace virtualdisplay::iddcx {

  /**
   * @brief Availability of every IddCx 1.10 DDI required by a console HDR adapter.
   */
  struct HdrDdiAvailability {
    bool monitor_update_modes2;  ///< True when extended target modes can be updated.
    bool swap_chain_buffer2;  ///< True when FP16-aware swapchain metadata can be acquired.
    bool parse_monitor_description2;  ///< True when extended monitor modes can be reported.
    bool query_target_info;  ///< True when target HDR capabilities can be reported.
    bool commit_modes2;  ///< True when committed HDR wire formats can be received.
    bool query_target_modes2;  ///< True when extended target modes can be reported.
    bool set_gamma_ramp;  ///< True when the 3x4 colorspace transform callback is available.
    bool set_default_hdr_metadata;  ///< True when the default HDR metadata callback is available.
  };

  /**
   * @brief Determine whether the complete console IddCx HDR contract is available.
   *
   * @param availability Runtime availability of the required IddCx 1.10 DDIs.
   * @return True only when advertising FP16 cannot cause Windows to call a missing DDI.
   */
  [[nodiscard]] constexpr bool SupportsConsoleHdr(const HdrDdiAvailability &availability) noexcept {
    return availability.monitor_update_modes2 &&
           availability.swap_chain_buffer2 &&
           availability.parse_monitor_description2 &&
           availability.query_target_info &&
           availability.commit_modes2 &&
           availability.query_target_modes2 &&
           availability.set_gamma_ramp &&
           availability.set_default_hdr_metadata;
  }

  /** @brief Default SDR white level required for HDR paths, in nits. */
  inline constexpr unsigned int kDefaultSdrWhiteLevel = 80;

  /** @brief Lowest SDR white level accepted by IddCx, in nits. */
  inline constexpr unsigned int kMinSdrWhiteLevel = 80;

  /** @brief Highest SDR white level accepted by IddCx, in nits. */
  inline constexpr unsigned int kMaxSdrWhiteLevel = 480;

  /**
   * @brief Determine whether a requested SDR white level is valid.
   *
   * @param level Requested white level in nits; zero selects the default.
   * @return True for zero or an IddCx-supported value from 80 through 480.
   */
  [[nodiscard]] constexpr bool IsValidSdrWhiteLevel(unsigned int level) noexcept {
    return level == 0 || (level >= kMinSdrWhiteLevel && level <= kMaxSdrWhiteLevel);
  }

  /**
   * @brief Normalize the optional SDR white level.
   *
   * @param level Requested white level in nits; zero selects the default.
   * @return The default for zero, otherwise the supplied valid level.
   */
  [[nodiscard]] constexpr unsigned int NormalizeSdrWhiteLevel(unsigned int level) noexcept {
    return level == 0 ? kDefaultSdrWhiteLevel : level;
  }

}  // namespace virtualdisplay::iddcx
