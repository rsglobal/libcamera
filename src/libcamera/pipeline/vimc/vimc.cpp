/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2018, Google Inc.
 *
 * vimc.cpp - Pipeline handler for the vimc device
 */

#include <algorithm>
#include <iomanip>
#include <map>
#include <math.h>
#include <tuple>

#include <linux/media-bus-format.h>
#include <linux/version.h>

#include <libcamera/camera.h>
#include <libcamera/control_ids.h>
#include <libcamera/controls.h>
#include <libcamera/formats.h>
#include <libcamera/ipa/ipa_interface.h>
#include <libcamera/ipa/ipa_module_info.h>
#include <libcamera/request.h>
#include <libcamera/stream.h>

#include "libcamera/internal/camera_sensor.h"
#include "libcamera/internal/device_enumerator.h"
#include "libcamera/internal/ipa_manager.h"
#include "libcamera/internal/log.h"
#include "libcamera/internal/media_device.h"
#include "libcamera/internal/pipeline_handler.h"
#include "libcamera/internal/utils.h"
#include "libcamera/internal/v4l2_subdevice.h"
#include "libcamera/internal/v4l2_videodevice.h"

#include <libcamera/ipa/vimc_ipa_interface.h>
#include <libcamera/ipa/vimc_ipa_proxy.h>

namespace libcamera {

LOG_DEFINE_CATEGORY(VIMC)

class VimcCameraData : public CameraData
{
public:
	VimcCameraData(PipelineHandler *pipe, MediaDevice *media)
		: CameraData(pipe), media_(media)
	{
	}

	int init();
	void bufferReady(FrameBuffer *buffer);

	MediaDevice *media_;
	std::unique_ptr<CameraSensor> sensor_;
	std::unique_ptr<V4L2Subdevice> debayer_;
	std::unique_ptr<V4L2Subdevice> scaler_;
	std::unique_ptr<V4L2VideoDevice> video_;
	std::unique_ptr<V4L2VideoDevice> raw_;
	Stream stream_;

	std::unique_ptr<ipa::vimc::IPAProxyVimc> ipa_;
};

class VimcCameraConfiguration : public CameraConfiguration
{
public:
	VimcCameraConfiguration(VimcCameraData *data);

	Status validate() override;

private:
	VimcCameraData *data_;
};

class PipelineHandlerVimc : public PipelineHandler
{
public:
	PipelineHandlerVimc(CameraManager *manager);

	CameraConfiguration *generateConfiguration(Camera *camera,
		const StreamRoles &roles) override;
	int configure(Camera *camera, CameraConfiguration *config) override;

	int exportFrameBuffers(Camera *camera, Stream *stream,
			       std::vector<std::unique_ptr<FrameBuffer>> *buffers) override;

	int start(Camera *camera, const ControlList *controls) override;
	void stop(Camera *camera) override;

	int queueRequestDevice(Camera *camera, Request *request) override;

	bool match(DeviceEnumerator *enumerator) override;

private:
	int processControls(VimcCameraData *data, Request *request);

	VimcCameraData *cameraData(const Camera *camera)
	{
		return static_cast<VimcCameraData *>(
			PipelineHandler::cameraData(camera));
	}
};

namespace {

static const std::map<PixelFormat, uint32_t> pixelformats{
	{ formats::RGB888, MEDIA_BUS_FMT_BGR888_1X24 },
	{ formats::BGR888, MEDIA_BUS_FMT_RGB888_1X24 },
};

} /* namespace */

VimcCameraConfiguration::VimcCameraConfiguration(VimcCameraData *data)
	: CameraConfiguration(), data_(data)
{
}

CameraConfiguration::Status VimcCameraConfiguration::validate()
{
	Status status = Valid;

	if (config_.empty())
		return Invalid;

	if (transform != Transform::Identity) {
		transform = Transform::Identity;
		status = Adjusted;
	}

	/* Cap the number of entries to the available streams. */
	if (config_.size() > 1) {
		config_.resize(1);
		status = Adjusted;
	}

	StreamConfiguration &cfg = config_[0];

	/* Adjust the pixel format. */
	const std::vector<libcamera::PixelFormat> formats = cfg.formats().pixelformats();
	if (std::find(formats.begin(), formats.end(), cfg.pixelFormat) == formats.end()) {
		LOG(VIMC, Debug) << "Adjusting format to BGR888";
		cfg.pixelFormat = formats::BGR888;
		status = Adjusted;
	}

	/* Clamp the size based on the device limits. */
	const Size size = cfg.size;

	/*
	 * The scaler hardcodes a x3 scale-up ratio, and the sensor output size
	 * is aligned to two pixels in both directions. The output width and
	 * height thus have to be multiples of 6.
	 */
	cfg.size.width = std::max(48U, std::min(4096U, cfg.size.width));
	cfg.size.height = std::max(48U, std::min(2160U, cfg.size.height));
	cfg.size.width -= cfg.size.width % 6;
	cfg.size.height -= cfg.size.height % 6;

	if (cfg.size != size) {
		LOG(VIMC, Debug)
			<< "Adjusting size to " << cfg.size.toString();
		status = Adjusted;
	}

	cfg.bufferCount = 4;

	V4L2DeviceFormat format;
	format.fourcc = data_->video_->toV4L2PixelFormat(cfg.pixelFormat);
	format.size = cfg.size;

	int ret = data_->video_->tryFormat(&format);
	if (ret)
		return Invalid;

	cfg.stride = format.planes[0].bpl;
	cfg.frameSize = format.planes[0].size;

	return status;
}

PipelineHandlerVimc::PipelineHandlerVimc(CameraManager *manager)
	: PipelineHandler(manager)
{
}

CameraConfiguration *PipelineHandlerVimc::generateConfiguration(Camera *camera,
	const StreamRoles &roles)
{
	VimcCameraData *data = cameraData(camera);
	CameraConfiguration *config = new VimcCameraConfiguration(data);

	if (roles.empty())
		return config;

	std::map<PixelFormat, std::vector<SizeRange>> formats;

	for (const auto &pixelformat : pixelformats) {
		/*
		 * Kernels prior to v5.7 incorrectly report support for RGB888,
		 * but it isn't functional within the pipeline.
		 */
		if (data->media_->version() < KERNEL_VERSION(5, 7, 0)) {
			if (pixelformat.first != formats::BGR888) {
				LOG(VIMC, Info)
					<< "Skipping unsupported pixel format "
					<< pixelformat.first.toString();
				continue;
			}
		}

		/* The scaler hardcodes a x3 scale-up ratio. */
		std::vector<SizeRange> sizes{
			SizeRange{ { 48, 48 }, { 4096, 2160 } }
		};
		formats[pixelformat.first] = sizes;
	}

	StreamConfiguration cfg(formats);

	cfg.pixelFormat = formats::BGR888;
	cfg.size = { 1920, 1080 };
	cfg.bufferCount = 4;

	config->addConfiguration(cfg);

	config->validate();

	return config;
}

int PipelineHandlerVimc::configure(Camera *camera, CameraConfiguration *config)
{
	VimcCameraData *data = cameraData(camera);
	StreamConfiguration &cfg = config->at(0);
	int ret;

	/* The scaler hardcodes a x3 scale-up ratio. */
	V4L2SubdeviceFormat subformat = {};
	subformat.mbus_code = MEDIA_BUS_FMT_SGRBG8_1X8;
	subformat.size = { cfg.size.width / 3, cfg.size.height / 3 };

	ret = data->sensor_->setFormat(&subformat);
	if (ret)
		return ret;

	ret = data->debayer_->setFormat(0, &subformat);
	if (ret)
		return ret;

	subformat.mbus_code = pixelformats.find(cfg.pixelFormat)->second;
	ret = data->debayer_->setFormat(1, &subformat);
	if (ret)
		return ret;

	ret = data->scaler_->setFormat(0, &subformat);
	if (ret)
		return ret;

	if (data->media_->version() >= KERNEL_VERSION(5, 6, 0)) {
		Rectangle crop{ 0, 0, subformat.size };
		ret = data->scaler_->setSelection(0, V4L2_SEL_TGT_CROP, &crop);
		if (ret)
			return ret;
	}

	subformat.size = cfg.size;
	ret = data->scaler_->setFormat(1, &subformat);
	if (ret)
		return ret;

	V4L2DeviceFormat format;
	format.fourcc = data->video_->toV4L2PixelFormat(cfg.pixelFormat);
	format.size = cfg.size;

	ret = data->video_->setFormat(&format);
	if (ret)
		return ret;

	if (format.size != cfg.size ||
	    format.fourcc != data->video_->toV4L2PixelFormat(cfg.pixelFormat))
		return -EINVAL;

	/*
	 * Format has to be set on the raw capture video node, otherwise the
	 * vimc driver will fail pipeline validation.
	 */
	format.fourcc = V4L2PixelFormat(V4L2_PIX_FMT_SGRBG8);
	format.size = { cfg.size.width / 3, cfg.size.height / 3 };

	ret = data->raw_->setFormat(&format);
	if (ret)
		return ret;

	cfg.setStream(&data->stream_);

	return 0;
}

int PipelineHandlerVimc::exportFrameBuffers(Camera *camera, Stream *stream,
					    std::vector<std::unique_ptr<FrameBuffer>> *buffers)
{
	VimcCameraData *data = cameraData(camera);
	unsigned int count = stream->configuration().bufferCount;

	return data->video_->exportBuffers(count, buffers);
}

int PipelineHandlerVimc::start(Camera *camera, [[maybe_unused]] const ControlList *controls)
{
	VimcCameraData *data = cameraData(camera);
	unsigned int count = data->stream_.configuration().bufferCount;

	int ret = data->video_->importBuffers(count);
	if (ret < 0)
		return ret;

	ret = data->ipa_->start();
	if (ret) {
		data->video_->releaseBuffers();
		return ret;
	}

	ret = data->video_->streamOn();
	if (ret < 0) {
		data->ipa_->stop();
		data->video_->releaseBuffers();
		return ret;
	}

	return 0;
}

void PipelineHandlerVimc::stop(Camera *camera)
{
	VimcCameraData *data = cameraData(camera);
	data->video_->streamOff();
	data->ipa_->stop();
	data->video_->releaseBuffers();
}

int PipelineHandlerVimc::processControls(VimcCameraData *data, Request *request)
{
	ControlList controls(data->sensor_->controls());

	for (auto it : request->controls()) {
		unsigned int id = it.first;
		unsigned int offset;
		uint32_t cid;

		if (id == controls::Brightness) {
			cid = V4L2_CID_BRIGHTNESS;
			offset = 128;
		} else if (id == controls::Contrast) {
			cid = V4L2_CID_CONTRAST;
			offset = 0;
		} else if (id == controls::Saturation) {
			cid = V4L2_CID_SATURATION;
			offset = 0;
		} else {
			continue;
		}

		int32_t value = lroundf(it.second.get<float>() * 128 + offset);
		controls.set(cid, std::clamp(value, 0, 255));
	}

	for (const auto &ctrl : controls)
		LOG(VIMC, Debug)
			<< "Setting control " << utils::hex(ctrl.first)
			<< " to " << ctrl.second.toString();

	int ret = data->sensor_->setControls(&controls);
	if (ret) {
		LOG(VIMC, Error) << "Failed to set controls: " << ret;
		return ret < 0 ? ret : -EINVAL;
	}

	return ret;
}

int PipelineHandlerVimc::queueRequestDevice(Camera *camera, Request *request)
{
	VimcCameraData *data = cameraData(camera);
	FrameBuffer *buffer = request->findBuffer(&data->stream_);
	if (!buffer) {
		LOG(VIMC, Error)
			<< "Attempt to queue request with invalid stream";

		return -ENOENT;
	}

	int ret = processControls(data, request);
	if (ret < 0)
		return ret;

	ret = data->video_->queueBuffer(buffer);
	if (ret < 0)
		return ret;

	return 0;
}

bool PipelineHandlerVimc::match(DeviceEnumerator *enumerator)
{
	DeviceMatch dm("vimc");

	dm.add("Raw Capture 0");
	dm.add("Raw Capture 1");
	dm.add("RGB/YUV Capture");
	dm.add("Sensor A");
	dm.add("Sensor B");
	dm.add("Debayer A");
	dm.add("Debayer B");
	dm.add("RGB/YUV Input");
	dm.add("Scaler");

	MediaDevice *media = acquireMediaDevice(enumerator, dm);
	if (!media)
		return false;

	std::unique_ptr<VimcCameraData> data = std::make_unique<VimcCameraData>(this, media);

	/* Locate and open the capture video node. */
	if (data->init())
		return false;

	data->ipa_ = IPAManager::createIPA<ipa::vimc::IPAProxyVimc>(this, 0, 0);
	if (data->ipa_ != nullptr) {
		std::string conf = data->ipa_->configurationFile("vimc.conf");
		data->ipa_->init(IPASettings{ conf, data->sensor_->model() });
	} else {
		LOG(VIMC, Warning) << "no matching IPA found";
	}

	/* Create and register the camera. */
	std::set<Stream *> streams{ &data->stream_ };
	std::shared_ptr<Camera> camera =
		Camera::create(this, data->sensor_->id(), streams);
	registerCamera(std::move(camera), std::move(data));

	return true;
}

int VimcCameraData::init()
{
	int ret;

	ret = media_->disableLinks();
	if (ret < 0)
		return ret;

	MediaLink *link = media_->link("Debayer B", 1, "Scaler", 0);
	if (!link)
		return -ENODEV;

	ret = link->setEnabled(true);
	if (ret < 0)
		return ret;

	/* Create and open the camera sensor, debayer, scaler and video device. */
	sensor_ = std::make_unique<CameraSensor>(media_->getEntityByName("Sensor B"));
	ret = sensor_->init();
	if (ret)
		return ret;

	debayer_ = V4L2Subdevice::fromEntityName(media_, "Debayer B");
	if (debayer_->open())
		return -ENODEV;

	scaler_ = V4L2Subdevice::fromEntityName(media_, "Scaler");
	if (scaler_->open())
		return -ENODEV;

	video_ = V4L2VideoDevice::fromEntityName(media_, "RGB/YUV Capture");
	if (video_->open())
		return -ENODEV;

	video_->bufferReady.connect(this, &VimcCameraData::bufferReady);

	raw_ = V4L2VideoDevice::fromEntityName(media_, "Raw Capture 1");
	if (raw_->open())
		return -ENODEV;

	/* Initialise the supported controls. */
	const ControlInfoMap &controls = sensor_->controls();
	ControlInfoMap::Map ctrls;

	for (const auto &ctrl : controls) {
		const ControlId *id;
		ControlInfo info;

		switch (ctrl.first->id()) {
		case V4L2_CID_BRIGHTNESS:
			id = &controls::Brightness;
			info = ControlInfo{ { -1.0f }, { 1.0f }, { 0.0f } };
			break;
		case V4L2_CID_CONTRAST:
			id = &controls::Contrast;
			info = ControlInfo{ { 0.0f }, { 2.0f }, { 1.0f } };
			break;
		case V4L2_CID_SATURATION:
			id = &controls::Saturation;
			info = ControlInfo{ { 0.0f }, { 2.0f }, { 1.0f } };
			break;
		default:
			continue;
		}

		ctrls.emplace(id, info);
	}

	controlInfo_ = std::move(ctrls);

	/* Initialize the camera properties. */
	properties_ = sensor_->properties();

	return 0;
}

void VimcCameraData::bufferReady(FrameBuffer *buffer)
{
	Request *request = buffer->request();

	/* Record the sensor's timestamp in the request metadata. */
	request->metadata().set(controls::SensorTimestamp,
				buffer->metadata().timestamp);

	pipe_->completeBuffer(request, buffer);
	pipe_->completeRequest(request);
}

REGISTER_PIPELINE_HANDLER(PipelineHandlerVimc)

} /* namespace libcamera */
