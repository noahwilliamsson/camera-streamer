#include "camera.h"

#include "device/device.h"
#include "device/device_list.h"
#include "device/buffer_list.h"
#include "device/buffer_lock.h"
#include "device/links.h"
#include "util/opts/log.h"
#include "util/opts/fourcc.h"

camera_t *camera_open(camera_options_t *options)
{
  camera_t *camera = calloc(1, sizeof(camera_t));
  camera->name = "CAMERA";
  camera->options = *options;
  camera->device_list = device_list_v4l2();

  if (camera_configure_input(camera) < 0) {
    goto error;
  }

  if (camera_set_params(camera) < 0) {
    goto error;
  }

  links_dump(camera->links);

  return camera;

error:
  camera_close(&camera);
  return NULL;
}

void camera_close(camera_t **camerap)
{
  if (!camerap || !*camerap)
    return;

  camera_t *camera = *camerap;
  *camerap = NULL;

  for (int i = MAX_DEVICES; i-- > 0; ) {
    if (camera->devices[i]) {
      device_close(camera->devices[i]);
      camera->devices[i] = NULL;
    }
  }

  for (int i = MAX_DEVICES; i-- > 0; ) {
    if (camera->links[i].callbacks.on_buffer) {
      camera->links[i].callbacks.on_buffer = NULL;
    }
  }

  device_list_free(camera->device_list);
  free(camera);
}

link_t *camera_ensure_capture(camera_t *camera, buffer_list_t *capture)
{
  for (int i = 0; i < camera->nlinks; i++) {
    if (camera->links[i].source == capture) {
      return &camera->links[i];
    }
  }

  link_t *link = &camera->links[camera->nlinks++];
  link->source = capture;
  return link;
}

void camera_capture_add_output(camera_t *camera, buffer_list_t *capture, buffer_list_t *output)
{
  link_t *link = camera_ensure_capture(camera, capture);

  int nsinks;
  for (nsinks = 0; link->sinks[nsinks]; nsinks++);
  link->sinks[nsinks] = output;
}

void camera_capture_set_callbacks(camera_t *camera, buffer_list_t *capture, link_callbacks_t callbacks)
{
  link_t *link = camera_ensure_capture(camera, capture);
  link->callbacks = callbacks;

  if (callbacks.buf_lock) {
    callbacks.buf_lock->buf_list = capture;
  }
}

int camera_set_params(camera_t *camera)
{
  device_set_fps(camera->camera, camera->options.fps);
  device_set_option_list(camera->camera, camera->options.options);
  device_set_option_list(camera->isp, camera->options.isp.options);

  if (camera->options.auto_focus) {
    device_set_option_string(camera->camera, "AfTrigger", "1");
  }

  // Set some defaults
  for (int i = 0; i < 2; i++) {
    device_set_option_list(camera->legacy_isp[i], camera->options.isp.options);
    device_set_option_list(camera->codec_jpeg[i], camera->options.jpeg.options);
    device_set_option_string(camera->codec_h264[i], "repeat_sequence_header", "1"); // required for force key support
    device_set_option_list(camera->codec_h264[i], camera->options.h264.options);
  }
  return 0;
}

int camera_run(camera_t *camera)
{
  bool running = false;
  return links_loop(camera->links, &running);
}
