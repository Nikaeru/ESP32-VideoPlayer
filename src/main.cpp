#include <Arduino.h>
#include <LittleFS.h>
#include <U8g2lib.h>
#include <WebServer.h>
#include <WiFi.h>

#include "web_page.h"

namespace {

constexpr char AP_SSID[] = "VideoPlayer-ESP32";
constexpr char AP_PASSWORD[] = "videoplay32";
constexpr char VIDEO_PATH[] = "/video.bap";
constexpr char TEMP_VIDEO_PATH[] = "/video.tmp";
constexpr char LEGACY_VIDEO_PATH[] = "/video.bin";

constexpr uint8_t DISPLAY_WIDTH = 72;
constexpr uint8_t DISPLAY_HEIGHT = 40;
constexpr size_t FRAME_SIZE = DISPLAY_WIDTH * DISPLAY_HEIGHT / 8;
constexpr size_t FILESYSTEM_RESERVE = 64 * 1024;
constexpr size_t HEADER_SIZE = 16;
constexpr uint8_t MIN_FPS = 5;
constexpr uint8_t MAX_FPS = 40;
constexpr float MIN_PLAYBACK_SPEED = 0.25f;
constexpr float MAX_PLAYBACK_SPEED = 2.0f;

struct __attribute__((packed)) BapHeader {
  char magic[4];
  uint8_t version;
  uint8_t width;
  uint8_t height;
  uint8_t fps;
  uint32_t frameCount;
  uint32_t payloadBytes;
};

static_assert(sizeof(BapHeader) == HEADER_SIZE, "BAP1 header must be 16 bytes");

U8G2_SSD1306_72X40_ER_F_HW_I2C display(
    U8G2_R0, U8X8_PIN_NONE, 6, 5);
WebServer server(80);

File videoFile;
File uploadFile;
BapHeader videoHeader{};
uint8_t frameBuffer[FRAME_SIZE];

bool filesystemReady = false;
bool videoReady = false;
bool playing = false;
bool uploading = false;
bool uploadSucceeded = false;
bool chunkSucceeded = false;
size_t uploadBytes = 0;
size_t expectedUploadBytes = 0;
uint32_t currentFrame = 0;
uint32_t nextFrameUs = 0;
float playbackSpeed = 1.0f;
String uploadError;

size_t maxVideoBytes() {
  if (!filesystemReady) {
    return 0;
  }

  const size_t total = LittleFS.totalBytes();
  return total > FILESYSTEM_RESERVE ? total - FILESYSTEM_RESERVE : 0;
}

void showAccessPoint() {
  display.clearBuffer();
  display.setFont(u8g2_font_4x6_tf);
  display.drawStr(0, 7, "VideoPlay Web");
  display.drawStr(0, 18, AP_SSID);
  display.drawStr(0, 29, "192.168.4.1");
  display.drawStr(0, 39, videoReady ? "Ready" : "Upload video");
  display.sendBuffer();
}

void showUploadState(const char *message) {
  display.clearBuffer();
  display.setFont(u8g2_font_5x7_tf);
  display.drawStr(0, 12, "Web upload");
  display.drawStr(0, 28, message);
  display.sendBuffer();
}

void closeVideo() {
  playing = false;
  videoReady = false;
  currentFrame = 0;
  if (videoFile) {
    videoFile.close();
  }
}

bool validateVideo(const char *path, BapHeader &header, String &error) {
  File file = LittleFS.open(path, FILE_READ);
  if (!file) {
    error = "File cannot be opened";
    return false;
  }

  if (file.size() < HEADER_SIZE ||
      file.read(reinterpret_cast<uint8_t *>(&header), HEADER_SIZE) != HEADER_SIZE) {
    file.close();
    error = "BAP1 header is incomplete";
    return false;
  }

  const bool magicValid = header.magic[0] == 'B' && header.magic[1] == 'A' &&
                          header.magic[2] == 'P' && header.magic[3] == '1';
  const uint64_t expectedPayload =
      static_cast<uint64_t>(header.frameCount) * FRAME_SIZE;
  const uint64_t expectedFileSize = HEADER_SIZE + expectedPayload;

  if (!magicValid || header.version != 1) {
    error = "Unsupported BAP format";
  } else if (header.width != DISPLAY_WIDTH || header.height != DISPLAY_HEIGHT) {
    error = "Invalid frame dimensions";
  } else if (header.fps < MIN_FPS || header.fps > MAX_FPS) {
    error = "FPS is outside the 5-40 range";
  } else if (header.frameCount == 0) {
    error = "Video has no frames";
  } else if (expectedPayload != header.payloadBytes) {
    error = "Payload size does not match frame count";
  } else if (expectedFileSize != file.size()) {
    error = "Uploaded file is incomplete";
  } else if (expectedFileSize > maxVideoBytes()) {
    error = "Video exceeds filesystem limit";
  } else {
    file.close();
    return true;
  }

  file.close();
  return false;
}

bool openVideo(bool autoplay) {
  closeVideo();

  String error;
  if (!filesystemReady || !LittleFS.exists(VIDEO_PATH) ||
      !validateVideo(VIDEO_PATH, videoHeader, error)) {
    Serial.printf("Video unavailable: %s\n", error.c_str());
    return false;
  }

  videoFile = LittleFS.open(VIDEO_PATH, FILE_READ);
  if (!videoFile || !videoFile.seek(HEADER_SIZE)) {
    closeVideo();
    Serial.println("Unable to seek to first video frame");
    return false;
  }

  videoReady = true;
  playing = autoplay;
  currentFrame = 0;
  nextFrameUs = micros();
  return true;
}

bool restartVideo(bool autoplay = true) {
  if (!videoReady || !videoFile) {
    return openVideo(autoplay);
  }

  if (!videoFile.seek(HEADER_SIZE)) {
    closeVideo();
    return false;
  }

  currentFrame = 0;
  playing = autoplay;
  nextFrameUs = micros();
  return true;
}

bool seekToFrame(uint32_t frameIndex) {
  if (!videoFile || videoHeader.frameCount == 0) {
    return false;
  }

  currentFrame = frameIndex % videoHeader.frameCount;
  const uint32_t offset = HEADER_SIZE + currentFrame * FRAME_SIZE;
  return videoFile.seek(offset);
}

void servicePlayback() {
  if (!videoReady || !playing || uploading || !videoFile) {
    return;
  }

  const uint32_t now = micros();
  if (static_cast<int32_t>(now - nextFrameUs) < 0) {
    return;
  }

  const uint32_t framePeriodUs = static_cast<uint32_t>(
      1000000.0f / (static_cast<float>(videoHeader.fps) * playbackSpeed));
  const uint32_t missedFrames = (now - nextFrameUs) / framePeriodUs;

  if (missedFrames > 0 &&
      !seekToFrame((currentFrame + missedFrames) % videoHeader.frameCount)) {
    closeVideo();
    showAccessPoint();
    return;
  }

  nextFrameUs += (missedFrames + 1) * framePeriodUs;

  if (currentFrame >= videoHeader.frameCount && !seekToFrame(0)) {
    closeVideo();
    showAccessPoint();
    return;
  }

  if (videoFile.read(frameBuffer, FRAME_SIZE) != FRAME_SIZE) {
    if (!seekToFrame(0) || videoFile.read(frameBuffer, FRAME_SIZE) != FRAME_SIZE) {
      closeVideo();
      showAccessPoint();
      return;
    }
  }

  display.clearBuffer();
  display.drawBitmap(0, 0, DISPLAY_WIDTH / 8, DISPLAY_HEIGHT, frameBuffer);
  display.sendBuffer();

  currentFrame++;
  if (currentFrame >= videoHeader.frameCount) {
    seekToFrame(0);
  }
}

void sendJsonMessage(int status, bool ok, const String &message) {
  String json;
  json.reserve(message.length() + 48);
  json = F("{\"ok\":");
  json += ok ? F("true") : F("false");
  json += F(",\"message\":\"");
  for (size_t i = 0; i < message.length(); ++i) {
    const char ch = message[i];
    if (ch == '\\' || ch == '"') {
      json += '\\';
    }
    json += ch;
  }
  json += F("\"}");
  server.send(status, "application/json; charset=utf-8", json);
}

void handleStatus() {
  String json;
  json.reserve(320);
  json = F("{\"playing\":");
  json += playing ? F("true") : F("false");
  json += F(",\"uploading\":");
  json += uploading ? F("true") : F("false");
  json += F(",\"videoReady\":");
  json += videoReady ? F("true") : F("false");
  json += F(",\"fps\":");
  json += videoReady ? String(videoHeader.fps) : F("0");
  json += F(",\"speed\":");
  json += String(playbackSpeed, 2);
  json += F(",\"effectiveFps\":");
  json += videoReady ? String(videoHeader.fps * playbackSpeed, 2) : F("0");
  json += F(",\"currentFrame\":");
  json += String(currentFrame);
  json += F(",\"frameCount\":");
  json += videoReady ? String(videoHeader.frameCount) : F("0");
  json += F(",\"videoBytes\":");
  json += videoReady ? String(HEADER_SIZE + videoHeader.payloadBytes) : F("0");
  json += F(",\"totalBytes\":");
  json += filesystemReady ? String(LittleFS.totalBytes()) : F("0");
  json += F(",\"usedBytes\":");
  json += filesystemReady ? String(LittleFS.usedBytes()) : F("0");
  json += F(",\"maxVideoBytes\":");
  json += String(maxVideoBytes());
  json += '}';
  server.send(200, "application/json; charset=utf-8", json);
}

void failUpload(const String &error) {
  if (uploadError.isEmpty()) {
    uploadError = error;
  }
}

void finalizeUpload() {
  if (uploadFile) {
    uploadFile.close();
  }
  uploading = false;

  if (!uploadError.isEmpty()) {
    LittleFS.remove(TEMP_VIDEO_PATH);
    showAccessPoint();
    return;
  }

  BapHeader header{};
  String error;
  if (!validateVideo(TEMP_VIDEO_PATH, header, error)) {
    failUpload(error);
    LittleFS.remove(TEMP_VIDEO_PATH);
    showAccessPoint();
    return;
  }

  if (LittleFS.exists(VIDEO_PATH)) {
    LittleFS.remove(VIDEO_PATH);
  }
  if (!LittleFS.rename(TEMP_VIDEO_PATH, VIDEO_PATH)) {
    failUpload("Unable to activate uploaded video");
    LittleFS.remove(TEMP_VIDEO_PATH);
    showAccessPoint();
    return;
  }

  uploadSucceeded = openVideo(true);
  if (!uploadSucceeded) {
    failUpload("Uploaded video could not be opened");
    showAccessPoint();
  }
}

void handleUploadData() {
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    closeVideo();
    LittleFS.remove(TEMP_VIDEO_PATH);
    LittleFS.remove(VIDEO_PATH);

    uploading = true;
    uploadSucceeded = false;
    uploadBytes = 0;
    uploadError = "";
    uploadFile = LittleFS.open(TEMP_VIDEO_PATH, FILE_WRITE);
    if (!uploadFile) {
      failUpload("Unable to create temporary file");
    }
    showUploadState("Receiving...");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadError.isEmpty()) {
      if (uploadBytes + upload.currentSize > maxVideoBytes()) {
        failUpload("Video exceeds filesystem limit");
      } else if (!uploadFile ||
                 uploadFile.write(upload.buf, upload.currentSize) != upload.currentSize) {
        failUpload("Flash write failed");
      } else {
        uploadBytes += upload.currentSize;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    finalizeUpload();
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    failUpload("Upload was interrupted");
    if (uploadFile) {
      uploadFile.close();
    }
    uploading = false;
    LittleFS.remove(TEMP_VIDEO_PATH);
    showAccessPoint();
  }
}

void handleUploadComplete() {
  if (uploadSucceeded) {
    sendJsonMessage(200, true, "Video uploaded and started");
  } else {
    const String error = uploadError.isEmpty() ? "Upload did not complete" : uploadError;
    sendJsonMessage(400, false, error);
  }
}

bool beginChunkedUpload(uint8_t fps, uint32_t frameCount, String &error) {
  const uint64_t payloadBytes = static_cast<uint64_t>(frameCount) * FRAME_SIZE;
  const uint64_t totalBytes = HEADER_SIZE + payloadBytes;
  if (fps < MIN_FPS || fps > MAX_FPS || frameCount == 0) {
    error = "Invalid FPS or frame count";
    return false;
  }
  if (payloadBytes > UINT32_MAX || totalBytes > maxVideoBytes()) {
    error = "Video exceeds filesystem limit";
    return false;
  }

  closeVideo();
  if (uploadFile) {
    uploadFile.close();
  }
  LittleFS.remove(TEMP_VIDEO_PATH);
  LittleFS.remove(VIDEO_PATH);

  uploadFile = LittleFS.open(TEMP_VIDEO_PATH, FILE_WRITE);
  if (!uploadFile) {
    error = "Unable to create temporary file";
    showAccessPoint();
    return false;
  }

  BapHeader header{{'B', 'A', 'P', '1'}, 1, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                   fps, frameCount, static_cast<uint32_t>(payloadBytes)};
  if (uploadFile.write(reinterpret_cast<const uint8_t *>(&header), HEADER_SIZE) !=
      HEADER_SIZE) {
    uploadFile.close();
    LittleFS.remove(TEMP_VIDEO_PATH);
    error = "Unable to write BAP1 header";
    showAccessPoint();
    return false;
  }

  uploadFile.flush();
  uploading = true;
  uploadSucceeded = false;
  chunkSucceeded = false;
  uploadBytes = 0;
  expectedUploadBytes = static_cast<size_t>(payloadBytes);
  uploadError = "";
  showUploadState("Streaming...");
  return true;
}

void cancelChunkedUpload(const String &reason) {
  if (uploadFile) {
    uploadFile.close();
  }
  uploading = false;
  uploadSucceeded = false;
  chunkSucceeded = false;
  uploadBytes = 0;
  expectedUploadBytes = 0;
  uploadError = reason;
  LittleFS.remove(TEMP_VIDEO_PATH);
  showAccessPoint();
}

void handleChunkData() {
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    chunkSucceeded = false;
    if (!uploading || !uploadFile) {
      failUpload("No active upload session");
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadError.isEmpty()) {
      if (uploadBytes + upload.currentSize > expectedUploadBytes) {
        failUpload("Received more video data than expected");
      } else if (!uploadFile ||
                 uploadFile.write(upload.buf, upload.currentSize) != upload.currentSize) {
        failUpload("Flash write failed");
      } else {
        uploadBytes += upload.currentSize;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.flush();
    }
    chunkSucceeded = uploadError.isEmpty();
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    cancelChunkedUpload("Chunk upload was interrupted");
  }
}

void handleChunkComplete() {
  if (chunkSucceeded) {
    sendJsonMessage(200, true, "Chunk stored");
  } else {
    const String error = uploadError.isEmpty() ? "Chunk was not stored" : uploadError;
    sendJsonMessage(400, false, error);
  }
}

void configureServer() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
  });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/upload", HTTP_POST, handleUploadComplete, handleUploadData);
  server.on("/api/upload/start", HTTP_POST, []() {
    if (!filesystemReady || !server.hasArg("fps") || !server.hasArg("frames")) {
      sendJsonMessage(400, false, "FPS and frame count are required");
      return;
    }

    String error;
    const uint8_t fps = static_cast<uint8_t>(server.arg("fps").toInt());
    const uint32_t frames = static_cast<uint32_t>(server.arg("frames").toInt());
    if (!beginChunkedUpload(fps, frames, error)) {
      sendJsonMessage(400, false, error);
      return;
    }
    sendJsonMessage(200, true, "Upload session started");
  });
  server.on("/api/upload/chunk", HTTP_POST, handleChunkComplete, handleChunkData);
  server.on("/api/upload/finish", HTTP_POST, []() {
    if (!uploading) {
      sendJsonMessage(409, false, "No active upload session");
      return;
    }
    if (uploadBytes != expectedUploadBytes) {
      const String error = "Upload is incomplete: " + String(uploadBytes) + "/" +
                           String(expectedUploadBytes);
      cancelChunkedUpload(error);
      sendJsonMessage(400, false, error);
      return;
    }

    finalizeUpload();
    if (uploadSucceeded) {
      sendJsonMessage(200, true, "Video uploaded and started");
    } else {
      const String error = uploadError.isEmpty() ? "Unable to finalize video" : uploadError;
      sendJsonMessage(400, false, error);
    }
  });
  server.on("/api/upload/cancel", HTTP_POST, []() {
    cancelChunkedUpload("Upload cancelled");
    sendJsonMessage(200, true, "Upload cancelled");
  });

  server.on("/api/play", HTTP_POST, []() {
    if (!videoReady && !openVideo(false)) {
      sendJsonMessage(409, false, "No valid video is stored");
      return;
    }
    playing = true;
    nextFrameUs = micros();
    sendJsonMessage(200, true, "Playback started");
  });

  server.on("/api/pause", HTTP_POST, []() {
    playing = false;
    sendJsonMessage(200, true, "Playback paused");
  });

  server.on("/api/restart", HTTP_POST, []() {
    if (!restartVideo(true)) {
      sendJsonMessage(409, false, "No valid video is stored");
      return;
    }
    sendJsonMessage(200, true, "Playback restarted");
  });

  server.on("/api/speed", HTTP_POST, []() {
    if (!server.hasArg("value")) {
      sendJsonMessage(400, false, "Speed value is required");
      return;
    }

    const float requestedSpeed = server.arg("value").toFloat();
    if (requestedSpeed < MIN_PLAYBACK_SPEED ||
        requestedSpeed > MAX_PLAYBACK_SPEED) {
      sendJsonMessage(400, false, "Speed must be between 0.25 and 2.0");
      return;
    }

    playbackSpeed = requestedSpeed;
    nextFrameUs = micros();
    sendJsonMessage(200, true, "Playback speed updated");
  });

  server.on("/api/video", HTTP_DELETE, []() {
    closeVideo();
    LittleFS.remove(TEMP_VIDEO_PATH);
    const bool removed = !LittleFS.exists(VIDEO_PATH) || LittleFS.remove(VIDEO_PATH);
    showAccessPoint();
    sendJsonMessage(removed ? 200 : 500, removed,
                    removed ? "Video deleted" : "Unable to delete video");
  });

  server.onNotFound([]() {
    sendJsonMessage(404, false, "Not found");
  });
  server.begin();
}

}  // namespace

void setup() {
  Serial.begin(115200);

  display.setI2CAddress(0x3C << 1);
  display.setBusClock(400000);
  display.begin();
  display.setPowerSave(0);

  filesystemReady = LittleFS.begin(true);
  if (!filesystemReady) {
    showUploadState("LittleFS error");
    Serial.println("LittleFS mount failed");
  } else if (LittleFS.exists(LEGACY_VIDEO_PATH)) {
    LittleFS.remove(LEGACY_VIDEO_PATH);
    Serial.println("Removed legacy /video.bin");
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  configureServer();

  if (!openVideo(true)) {
    showAccessPoint();
  }

  Serial.printf("Wi-Fi: %s\n", AP_SSID);
  Serial.printf("Open: http://%s\n", WiFi.softAPIP().toString().c_str());
}

void loop() {
  server.handleClient();
  servicePlayback();
  delay(1);
}
