extern "C" {

#include "util/http/http.h"
#include "device/buffer.h"
#include "device/buffer_list.h"
#include "device/buffer_lock.h"
#include "device/device.h"
#include "util/opts/log.h"
#include "util/opts/fourcc.h"
#include "util/opts/control.h"
#include "output/output.h"
#include "rtsp.h"

};

#ifdef USE_RTSP

#include <BasicUsageEnvironment.hh>
#include <RTSPServerSupportingHTTPStreaming.hh>
#include <OnDemandServerMediaSubsession.hh>
#include <H264VideoStreamFramer.hh>
#include <H264VideoRTPSink.hh>

static pthread_t rtsp_thread;
static pthread_mutex_t rtsp_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static class DynamicH264Stream *rtsp_streams;

static const char *stream_name = "stream.h264";
static const char *stream_low_res_name = "stream_low_res.h264";

class DynamicH264Stream : public FramedSource
{
public:
  DynamicH264Stream(UsageEnvironment& env, Boolean lowResMode)
    : FramedSource(env), fHaveStartedReading(False), fLowResMode(lowResMode)
  {
  }

  void doGetNextFrame()
  {
    pthread_mutex_lock(&rtsp_lock);
    if (!fHaveStartedReading) {
      pNextStream = rtsp_streams;
      rtsp_streams = this;
      fHaveStartedReading = True;
    }
    pthread_mutex_unlock(&rtsp_lock);
  }

  void doStopGettingFrames()
  {
    pthread_mutex_lock(&rtsp_lock);
    if (fHaveStartedReading) {
      DynamicH264Stream **streamp = &rtsp_streams;
      while (*streamp) {
        if (*streamp == this) {
          *streamp = pNextStream;
          pNextStream = NULL;
          break;
        }
        streamp = &(*streamp)->pNextStream;
      }
      fHaveStartedReading = False;
    }
    pthread_mutex_unlock(&rtsp_lock);
  }

  void receiveData(buffer_t *buf, bool lowResMode)
  {
    if (!isCurrentlyAwaitingData()) {
      return; // we're not ready for the data yet
    }

    if (fLowResMode != lowResMode) {
      return;
    }

    if (h264_is_key_frame(buf)) {
      fHadKeyFrame = true;
    }

    if (!fRequestedKeyFrame) {
      if (!fHadKeyFrame) {
        printf("device_video_force_key: %p\n", this);
        device_video_force_key(buf->buf_list->dev);
      }

      fRequestedKeyFrame = true;
    }

    if (!fHadKeyFrame) {
      return;
    }

    if (buf->used > fMaxSize) {
      fNumTruncatedBytes = buf->used - fMaxSize;
      fFrameSize = fMaxSize;
    } else {
      fNumTruncatedBytes = 0;
      fFrameSize = buf->used;
    }

    memcpy(fTo, buf->start, fFrameSize);

    // Tell our client that we have new data:
    afterGetting(this); // we're preceded by a net read; no infinite recursion
  }

private:
  Boolean fHaveStartedReading;
  Boolean fHadKeyFrame;
  Boolean fRequestedKeyFrame;
  Boolean fLowResMode;

public:
  DynamicH264Stream *pNextStream;
};

class DynamicH264VideoFileServerMediaSubsession : public OnDemandServerMediaSubsession
{
public:
  DynamicH264VideoFileServerMediaSubsession(UsageEnvironment& env, Boolean reuseFirstSource, Boolean lowResMode)
    : OnDemandServerMediaSubsession(env, reuseFirstSource), fLowResMode(lowResMode)
  {
  }

  virtual FramedSource* createNewStreamSource(unsigned clientSessionId, unsigned& estBitrate)
  {
    estBitrate = 500; // kbps, estimate
    return H264VideoStreamFramer::createNew(envir(), new DynamicH264Stream(envir(), fLowResMode));
  }

  virtual RTPSink* createNewRTPSink(Groupsock* rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic, FramedSource* /*inputSource*/)
  {
    return H264VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic);
  }

private:
  Boolean fLowResMode;
};

class DynamicRTSPServer: public RTSPServerSupportingHTTPStreaming
{
public:
  static DynamicRTSPServer* createNew(UsageEnvironment& env, Port ourPort,
				      UserAuthenticationDatabase* authDatabase,
				      unsigned reclamationTestSeconds = 65)
  {
    int ourSocket = setUpOurSocket(env, ourPort);
    if (ourSocket == -1) return NULL;

    return new DynamicRTSPServer(env, ourSocket, ourPort, authDatabase, reclamationTestSeconds);
  }

protected:
  DynamicRTSPServer(UsageEnvironment& env, int ourSocket, Port ourPort,
		    UserAuthenticationDatabase* authDatabase, unsigned reclamationTestSeconds)
    : RTSPServerSupportingHTTPStreaming(env, ourSocket, ourPort, authDatabase, reclamationTestSeconds)
  {
  }

  // called only by createNew();
  virtual ~DynamicRTSPServer()
  {
  }

protected: // redefined virtual functions
  virtual ServerMediaSession* lookupServerMediaSession(char const* streamName, Boolean isFirstLookupInSession)
  {
    bool lowResMode = false;

    if (strcmp(streamName, stream_name) == 0) {
      LOG_INFO(NULL, "Requesting %s stream...", streamName);
    } else if (strcmp(streamName, stream_low_res_name) == 0) {
      LOG_INFO(NULL, "Requesting %s stream (low resolution mode)...", streamName);
      lowResMode = true;
    } else {
      LOG_INFO(NULL, "No stream available: '%s'", streamName);
      return NULL;
    }

    auto sms = RTSPServer::lookupServerMediaSession(streamName);

    if (sms && isFirstLookupInSession) { 
      // Remove the existing "ServerMediaSession" and create a new one, in case the underlying
      // file has changed in some way:
      removeServerMediaSession(sms); 
      sms = NULL;
    }

    sms = ServerMediaSession::createNew(envir(), streamName, streamName, "streamed by the LIVE555 Media Server");;
    OutPacketBuffer::maxSize = 2000000; // allow for some possibly large H.264 frames

    auto subsession = new DynamicH264VideoFileServerMediaSubsession(envir(), false, lowResMode);
    sms->addSubsession(subsession);
    addServerMediaSession(sms);
    return sms;
  }
};

static void *rtsp_server_thread(void *opaque)
{
  UsageEnvironment* env = (UsageEnvironment*)opaque;
  env->taskScheduler().doEventLoop(); // does not return
  return NULL;
}

static bool rtsp_h264_needs_buffer(buffer_lock_t *buf_lock)
{
  return rtsp_streams != NULL;
}

static void rtsp_h264_capture(buffer_lock_t *buf_lock, buffer_t *buf)
{
  pthread_mutex_lock(&rtsp_lock);
  for (DynamicH264Stream *stream = rtsp_streams; stream; stream = stream->pNextStream) {
    stream->receiveData(buf, false);

    if (!http_h264_lowres.buf_list) {
      stream->receiveData(buf, true);
    }
  }
  pthread_mutex_unlock(&rtsp_lock);
}

static void rtsp_h264_low_res_capture(buffer_lock_t *buf_lock, buffer_t *buf)
{
  pthread_mutex_lock(&rtsp_lock);
  for (DynamicH264Stream *stream = rtsp_streams; stream; stream = stream->pNextStream) {
    stream->receiveData(buf, true);
  }
  pthread_mutex_unlock(&rtsp_lock);
}

extern "C" int rtsp_server(rtsp_options_t *options)
{
  // Begin by setting up our usage environment:
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);
  UserAuthenticationDatabase* authDB = NULL;

#ifdef ACCESS_CONTROL
  // To implement client access control to the RTSP server, do the following:
  authDB = new UserAuthenticationDatabase;
  authDB->addUserRecord("username1", "password1"); // replace these with real strings
  // Repeat the above with each <username>, <password> that you wish to allow
  // access to the server.
#endif

  RTSPServer* rtspServer;
  rtspServer = DynamicRTSPServer::createNew(*env, options->port, authDB);
  if (rtspServer == NULL) {
    LOG_ERROR(NULL, "Failed to create RTSP server: %s", env->getResultMsg());
    return -1;
  }
  LOG_INFO(NULL, "Running RTSP server on '%d'", options->port);

  // if (rtspServer->setUpTunnelingOverHTTP(80) || rtspServer->setUpTunnelingOverHTTP(8000) || rtspServer->setUpTunnelingOverHTTP(8080)) {
  //   LOG_INFO(NULL, "Running RTSP-over-HTTP tunneling on '%d'", rtspServer->httpServerPortNum());
  //   *env << "(We use port " << rtspServer->httpServerPortNum() << " for optional RTSP-over-HTTP tunneling, or for HTTP live streaming (for indexed Transport Stream files only).)\n";
  // } else {
  //   LOG_INFO(NULL, "The RTSP-over-HTTP is not available.");
  // }

  buffer_lock_register_check_streaming(&http_h264, rtsp_h264_needs_buffer);
  buffer_lock_register_notify_buffer(&http_h264, rtsp_h264_capture);
  buffer_lock_register_check_streaming(&http_h264_lowres, rtsp_h264_needs_buffer);
  buffer_lock_register_notify_buffer(&http_h264_lowres, rtsp_h264_low_res_capture);

  pthread_create(&rtsp_thread, NULL, rtsp_server_thread, env);
  return 0;

error:
  return -1;
}

#else // USE_RTSP

extern "C" int rtsp_server()
{
  return 0;
}

#endif // USE_RTSP