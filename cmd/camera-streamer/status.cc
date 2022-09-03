extern "C" {

#include "util/http/http.h"
#include "util/opts/fourcc.h"
#include "device/buffer_list.h"
#include "device/buffer_lock.h"
#include "device/camera/camera.h"
#include "output/output.h"

extern camera_t *camera;

};

#include <nlohmann/json.hpp>

nlohmann::json serialize_buf_list(buffer_list_t *buf_list)
{
  if (!buf_list)
    return false;

  nlohmann::json output;
  output["width"] = buf_list->fmt.width;
  output["height"] = buf_list->fmt.height;
  output["format"] = fourcc_to_string(buf_list->fmt.format).buf;
  output["nbufs"] = buf_list->nbufs;

  return output;
}

extern "C" void camera_status_json(http_worker_t *worker, FILE *stream)
{
  nlohmann::json outputs;
  outputs["h264"]["high_res"] = serialize_buf_list(http_h264.buf_list);
  outputs["h264"]["low_res"] = serialize_buf_list(http_h264_lowres.buf_list);
  outputs["jpeg"]["high_res"] = serialize_buf_list(http_jpeg.buf_list);
  outputs["jpeg"]["low_res"] = serialize_buf_list(http_jpeg_lowres.buf_list);

  nlohmann::json devices;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!camera->devices[i])
      continue;

    device_t *device = camera->devices[i];
    nlohmann::json device_json;
    device_json["name"] = device->name;
    device_json["path"] = device->path;
    device_json["allow_dma"] = device->opts.allow_dma;
    device_json["output"] = serialize_buf_list(device->output_list);
    for (int j = 0; j < device->n_capture_list; j++) {
      device_json["captures"][j] = serialize_buf_list(device->capture_lists[j]);
    }
    devices += device_json;
  }

  nlohmann::json message;
  message["outputs"] = outputs;
  message["devices"] = devices;

  http_write_response(stream, "200 OK", "application/json", message.dump().c_str(), 0);
}
