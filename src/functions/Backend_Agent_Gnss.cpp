#include "functions/Backend_Agent.h"
#include "Backend_Agent_Internal.h"
#include "functions/Unicore_Readback.h"
#include "functions/RTCM_Receiver.h"
#include "Prog_Config.h"
#include <cstring>

using namespace BackendAgentInternal;
extern SemaphoreHandle_t rtcmBufferMutex;

bool BackendAgent::refreshReceiverReadback(String &error) {
    lastReceiverPollMs_ = millis();
    String mode;
    const bool success = UnicoreReadback::readMode(mode, error);
    DynamicJsonDocument readbackDoc(1024);
    readbackDoc["state"] = success ? "ok" : "error";
    readbackDoc["chip"] = "Unicorecomm";
    readbackDoc["revision"] = ++receiverBaseReadbackRevision_;
    readbackDoc["mode_raw"] = mode;
    readbackDoc["mode_kind"] = success ? UnicoreReadback::modeKind(mode) : "unknown";
    readbackDoc["query"] = "MODE";
    if (success) {
        if (String(UnicoreReadback::modeKind(mode)) == "survey_in") {
            if (!surveyInMonitoring_) {
                surveyInMonitoring_ = true;
                surveyInStartedMs_ = 0;
                surveyInTargetSeconds_ = 0;
                surveyInBaselineGgaCount_ = getGnssTelemetrySnapshot().ggaCount;
                lastSurveyGgaPollMs_ = 0;
                lastSurveyLoggedState_ = "";
            }
        } else {
            surveyInMonitoring_ = false;
        }
        JsonObject bestPosition = readbackDoc.createNestedObject("bestpos");
        String positionError;
        if (!UnicoreReadback::readBestPosition(bestPosition, positionError)) {
            surveyInReadbackHasPosition_ = false;
            bestPosition["state"] = "unavailable";
            bestPosition["error"] = positionError;
        } else {
            surveyInReadbackHasPosition_ = true;
            surveyInReadbackReady_ = String(bestPosition["position_type"] | "") == "FIXEDPOS";
        }
        if (surveyInMonitoring_ && surveyInReadbackHasPosition_ && surveyInReadbackReady_
            && (surveyInResultPending_ || surveyInResult_.length() <= 2)) {
            DynamicJsonDocument savedSurvey(1024);
            savedSurvey["bestpos"].set(bestPosition);
            savedSurvey["mode_raw"] = mode;
            savedSurvey["recorded_at"] = epochString().toInt();
            surveyInResult_.clear();
            serializeJson(savedSurvey, surveyInResult_);
            surveyInResultPending_ = false;
            saveStatePreferences();
        }

    } else {
        surveyInReadbackHasPosition_ = false;
        readbackDoc["error"] = error;
    }
    receiverBaseReadback_.clear();
    serializeJson(readbackDoc, receiverBaseReadback_);
    receiverBaseReadbackMs_ = millis();
    markStatusDirty();
    return success;
}

void BackendAgent::pollGnssStatus() {
    if (sensorType_ != "Unicorecomm" || workflowActive() || stationLock_.all) return;
    const uint32_t now = millis();
    if ((lastReceiverPollMs_ == 0 && now >= 5000UL)
        || (lastReceiverPollMs_ != 0 && now - lastReceiverPollMs_ >= 30000UL)) {
        String error;
        refreshReceiverReadback(error);
    }

    // MODE/BESTNAV are read-only receiver logs; do not request NMEA output.
}
