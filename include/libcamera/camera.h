/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2018, Google Inc.
 *
 * camera.h - Camera object interface
 */
#ifndef __LIBCAMERA_CAMERA_H__
#define __LIBCAMERA_CAMERA_H__

#include <memory>
#include <set>
#include <stdint.h>
#include <string>

#include <libcamera/class.h>
#include <libcamera/controls.h>
#include <libcamera/object.h>
#include <libcamera/request.h>
#include <libcamera/signal.h>
#include <libcamera/stream.h>
#include <libcamera/transform.h>

namespace libcamera {

class FrameBuffer;
class FrameBufferAllocator;
class PipelineHandler;
class Request;

class CameraConfiguration
{
public:
	enum Status {
		Valid,
		Adjusted,
		Invalid,
	};

	using iterator = std::vector<StreamConfiguration>::iterator;
	using const_iterator = std::vector<StreamConfiguration>::const_iterator;

	virtual ~CameraConfiguration();

	void addConfiguration(const StreamConfiguration &cfg);
	virtual Status validate() = 0;

	StreamConfiguration &at(unsigned int index);
	const StreamConfiguration &at(unsigned int index) const;
	StreamConfiguration &operator[](unsigned int index)
	{
		return at(index);
	}
	const StreamConfiguration &operator[](unsigned int index) const
	{
		return at(index);
	}

	iterator begin();
	const_iterator begin() const;
	iterator end();
	const_iterator end() const;

	bool empty() const;
	std::size_t size() const;

	Transform transform;

protected:
	CameraConfiguration();

	std::vector<StreamConfiguration> config_;
};

class Camera final : public Object, public std::enable_shared_from_this<Camera>,
		     public Extensible
{
	LIBCAMERA_DECLARE_PRIVATE()

public:
	static std::shared_ptr<Camera> create(PipelineHandler *pipe,
					      const std::string &id,
					      const std::set<Stream *> &streams);

	const std::string &id() const;

	Signal<Request *, FrameBuffer *> bufferCompleted;
	Signal<Request *> requestCompleted;
	Signal<Camera *> disconnected;

	int acquire();
	int release();

	const ControlInfoMap &controls() const;
	const ControlList &properties() const;

	const std::set<Stream *> &streams() const;
	std::unique_ptr<CameraConfiguration> generateConfiguration(const StreamRoles &roles = {});
	int configure(CameraConfiguration *config);

	std::unique_ptr<Request> createRequest(uint64_t cookie = 0);
	int queueRequest(Request *request);

	int start(const ControlList *controls = nullptr);
	int stop();

private:
	LIBCAMERA_DISABLE_COPY(Camera)

	Camera(PipelineHandler *pipe, const std::string &id,
	       const std::set<Stream *> &streams);
	~Camera();

	friend class PipelineHandler;
	void disconnect();
	void requestComplete(Request *request);

	friend class FrameBufferAllocator;
	int exportFrameBuffers(Stream *stream,
			       std::vector<std::unique_ptr<FrameBuffer>> *buffers);
};

} /* namespace libcamera */

#endif /* __LIBCAMERA_CAMERA_H__ */
