#ifndef COMMAND_BUILDER_H
#define COMMAND_BUILDER_H

#include <Arduino.h>
#include <cstdint>
#include <vector>

namespace UbxCmdBuilder
{

    using Command = std::vector<uint8_t>;
    using CommandList = std::vector<Command>;

    enum class OutputMode
    {
        DiagnosticsRtcm,
        RtcmOnly
    };
    enum class MsmLevel : uint8_t
    {
        Msm4 = 4,
        Msm7 = 7,
        Both = 0
    };

    struct GnssOptions
    {
        bool gps = true;
        bool glo = true;
        bool gal = true;
        bool bds = true;
        bool qzss = false;
        bool sbas = false;
        OutputMode outputMode = OutputMode::DiagnosticsRtcm;
        MsmLevel msmLevel = MsmLevel::Msm4;
        std::vector<String> ports = {"USB"};
    };

    GnssOptions normalizeGnssOptions(GnssOptions options = {});
    Command buildUbloxOutputConfigCommand(const GnssOptions &options = {});
    CommandList buildBaseSurveyInCommand(uint32_t duration,
                                         float accuracy, const GnssOptions &options = {});
    CommandList buildBaseFixedLlaCommand(double lat, double lon,
                                         double alt, float accuracy,
                                         const GnssOptions &options = {});
    CommandList buildBaseRtcmOutputCommand(const String &message,
                                           const String &port);
    CommandList buildGeotekLteUnicoreConfig(const String &setupMethod, uint32_t duration = 60,
                                            double lat = 0, double lon = 0, double alt = 0);
    Command commandListToBytes(const CommandList &commands);

    #if PROGRAM_DEBUG
    String debugCommand(const Command &command);
    #endif

} // namespace UbxCmdBuilder

#endif // COMMAND_BUILDER_H
