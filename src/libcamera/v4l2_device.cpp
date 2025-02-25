/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * v4l2_device.cpp - Common base for V4L2 video devices and subdevices
 */

#include "libcamera/internal/v4l2_device.h"

#include <fcntl.h>
#include <iomanip>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "libcamera/internal/event_notifier.h"
#include "libcamera/internal/log.h"
#include "libcamera/internal/sysfs.h"
#include "libcamera/internal/utils.h"

/**
 * \file v4l2_device.h
 * \brief Common base for V4L2 devices and subdevices
 */

namespace libcamera {

LOG_DEFINE_CATEGORY(V4L2)

namespace {

ControlType v4l2CtrlType(uint32_t ctrlType)
{
	switch (ctrlType) {
	case V4L2_CTRL_TYPE_U8:
		return ControlTypeByte;

	case V4L2_CTRL_TYPE_BOOLEAN:
		return ControlTypeBool;

	case V4L2_CTRL_TYPE_INTEGER:
		return ControlTypeInteger32;

	case V4L2_CTRL_TYPE_INTEGER64:
		return ControlTypeInteger64;

	case V4L2_CTRL_TYPE_MENU:
	case V4L2_CTRL_TYPE_BUTTON:
	case V4L2_CTRL_TYPE_BITMASK:
	case V4L2_CTRL_TYPE_INTEGER_MENU:
		/*
		 * More precise types may be needed, for now use a 32-bit
		 * integer type.
		 */
		return ControlTypeInteger32;

	default:
		return ControlTypeNone;
	}
}

std::unique_ptr<ControlId> v4l2ControlId(const v4l2_query_ext_ctrl &ctrl)
{
	const size_t len = strnlen(ctrl.name, sizeof(ctrl.name));
	const std::string name(static_cast<const char *>(ctrl.name), len);
	const ControlType type = v4l2CtrlType(ctrl.type);

	return std::make_unique<ControlId>(ctrl.id, name, type);
}

ControlInfo v4l2ControlInfo(const v4l2_query_ext_ctrl &ctrl)
{
	switch (ctrl.type) {
	case V4L2_CTRL_TYPE_U8:
		return ControlInfo(static_cast<uint8_t>(ctrl.minimum),
				   static_cast<uint8_t>(ctrl.maximum),
				   static_cast<uint8_t>(ctrl.default_value));

	case V4L2_CTRL_TYPE_BOOLEAN:
		return ControlInfo(static_cast<bool>(ctrl.minimum),
				   static_cast<bool>(ctrl.maximum),
				   static_cast<bool>(ctrl.default_value));

	case V4L2_CTRL_TYPE_INTEGER64:
		return ControlInfo(static_cast<int64_t>(ctrl.minimum),
				   static_cast<int64_t>(ctrl.maximum),
				   static_cast<int64_t>(ctrl.default_value));

	default:
		return ControlInfo(static_cast<int32_t>(ctrl.minimum),
				   static_cast<int32_t>(ctrl.maximum),
				   static_cast<int32_t>(ctrl.default_value));
	}
}
} /* namespace */

/**
 * \class V4L2Device
 * \brief Base class for V4L2VideoDevice and V4L2Subdevice
 *
 * The V4L2Device class groups together the methods and fields common to
 * both the V4L2VideoDevice and V4L2Subdevice classes, and provides a base
 * class with methods to open and close the device node associated with the
 * device and to perform IOCTL system calls on it.
 *
 * The V4L2Device class cannot be instantiated directly, as its constructor
 * is protected. Users should instead create instances of one the derived
 * classes to model either a V4L2 video device or a V4L2 subdevice.
 */

/**
 * \brief Construct a V4L2Device
 * \param[in] deviceNode The device node filesystem path
 *
 * Initialize the file descriptor to -1 and store the \a deviceNode to be used
 * at open() time, and the \a logTag to prefix log messages with.
 */
V4L2Device::V4L2Device(const std::string &deviceNode)
	: deviceNode_(deviceNode), fd_(-1), fdEventNotifier_(nullptr),
	  frameStartEnabled_(false)
{
}

/**
 * \brief Destroy a V4L2Device
 */
V4L2Device::~V4L2Device()
{
}

/**
 * \brief Open a V4L2 device node
 * \param[in] flags Access mode flags
 *
 * Open the device node path with the provided access mode \a flags and
 * initialize the file descriptor, which was initially set to -1.
 *
 * \return 0 on success or a negative error code otherwise
 */
int V4L2Device::open(unsigned int flags)
{
	if (isOpen()) {
		LOG(V4L2, Error) << "Device already open";
		return -EBUSY;
	}

	int ret = syscall(SYS_openat, AT_FDCWD, deviceNode_.c_str(), flags);
	if (ret < 0) {
		ret = -errno;
		LOG(V4L2, Error) << "Failed to open V4L2 device: "
				 << strerror(-ret);
		return ret;
	}

	setFd(ret);

	listControls();

	return 0;
}

/**
 * \brief Set the file descriptor of a V4L2 device
 * \param[in] fd The file descriptor handle
 *
 * This method allows a device to provide an already opened file descriptor
 * referring to the V4L2 device node, instead of opening it with open(). This
 * can be used for V4L2 M2M devices where a single video device node is used for
 * both the output and capture devices, or when receiving an open file
 * descriptor in a context that doesn't have permission to open the device node
 * itself.
 *
 * This method and the open() method are mutually exclusive, only one of the two
 * shall be used for a V4L2Device instance.
 *
 * \return 0 on success or a negative error code otherwise
 */
int V4L2Device::setFd(int fd)
{
	if (isOpen())
		return -EBUSY;

	fd_ = fd;

	fdEventNotifier_ = new EventNotifier(fd_, EventNotifier::Exception);
	fdEventNotifier_->activated.connect(this, &V4L2Device::eventAvailable);
	fdEventNotifier_->setEnabled(false);

	return 0;
}

/**
 * \brief Close the device node
 *
 * Reset the file descriptor to -1
 */
void V4L2Device::close()
{
	if (!isOpen())
		return;

	delete fdEventNotifier_;

	if (::close(fd_) < 0)
		LOG(V4L2, Error) << "Failed to close V4L2 device: "
				 << strerror(errno);
	fd_ = -1;
}

/**
 * \fn V4L2Device::isOpen()
 * \brief Check if the V4L2 device node is open
 * \return True if the V4L2 device node is open, false otherwise
 */

/**
 * \fn V4L2Device::controls()
 * \brief Retrieve the supported V4L2 controls and their information
 * \return A map of the V4L2 controls supported by the device
 */

/**
 * \brief Read controls from the device
 * \param[in] ids The list of controls to read, specified by their ID
 *
 * This method reads the value of all controls contained in \a ids, and returns
 * their values as a ControlList.
 *
 * If any control in \a ids is not supported by the device, is disabled (i.e.
 * has the V4L2_CTRL_FLAG_DISABLED flag set), or if any other error occurs
 * during validation of the requested controls, no control is read and this
 * method returns an empty control list.
 *
 * \return The control values in a ControlList on success, or an empty list on
 * error
 */
ControlList V4L2Device::getControls(const std::vector<uint32_t> &ids)
{
	if (ids.empty())
		return {};

	ControlList ctrls{ controls_ };

	for (uint32_t id : ids) {
		const auto iter = controls_.find(id);
		if (iter == controls_.end()) {
			LOG(V4L2, Error)
				<< "Control " << utils::hex(id) << " not found";
			return {};
		}

		ctrls.set(id, {});
	}

	std::vector<v4l2_ext_control> v4l2Ctrls(ids.size());
	memset(v4l2Ctrls.data(), 0, sizeof(v4l2_ext_control) * ctrls.size());

	unsigned int i = 0;
	for (auto &ctrl : ctrls) {
		unsigned int id = ctrl.first;
		const struct v4l2_query_ext_ctrl &info = controlInfo_[id];

		v4l2_ext_control &v4l2Ctrl = v4l2Ctrls[i++];
		v4l2Ctrl.id = id;

		if (info.flags & V4L2_CTRL_FLAG_HAS_PAYLOAD) {
			ControlType type;

			switch (info.type) {
			case V4L2_CTRL_TYPE_U8:
				type = ControlTypeByte;
				break;

			default:
				LOG(V4L2, Error)
					<< "Unsupported payload control type "
					<< info.type;
				return {};
			}

			ControlValue &value = ctrl.second;
			value.reserve(type, true, info.elems);
			Span<uint8_t> data = value.data();

			v4l2Ctrl.p_u8 = data.data();
			v4l2Ctrl.size = data.size();
		}
	}

	struct v4l2_ext_controls v4l2ExtCtrls = {};
	v4l2ExtCtrls.which = V4L2_CTRL_WHICH_CUR_VAL;
	v4l2ExtCtrls.controls = v4l2Ctrls.data();
	v4l2ExtCtrls.count = v4l2Ctrls.size();

	int ret = ioctl(VIDIOC_G_EXT_CTRLS, &v4l2ExtCtrls);
	if (ret) {
		unsigned int errorIdx = v4l2ExtCtrls.error_idx;

		/* Generic validation error. */
		if (errorIdx == 0 || errorIdx >= v4l2Ctrls.size()) {
			LOG(V4L2, Error) << "Unable to read controls: "
					 << strerror(-ret);
			return {};
		}

		/* A specific control failed. */
		LOG(V4L2, Error) << "Unable to read control " << errorIdx
				 << ": " << strerror(-ret);

		v4l2Ctrls.resize(errorIdx);
	}

	updateControls(&ctrls, v4l2Ctrls);

	return ctrls;
}

/**
 * \brief Write controls to the device
 * \param[in] ctrls The list of controls to write
 *
 * This method writes the value of all controls contained in \a ctrls, and
 * stores the values actually applied to the device in the corresponding
 * \a ctrls entry.
 *
 * If any control in \a ctrls is not supported by the device, is disabled (i.e.
 * has the V4L2_CTRL_FLAG_DISABLED flag set), is read-only, if any other error
 * occurs during validation of the requested controls, no control is written and
 * this method returns -EINVAL.
 *
 * If an error occurs while writing the controls, the index of the first
 * control that couldn't be written is returned. All controls below that index
 * are written and their values are updated in \a ctrls, while all other
 * controls are not written and their values are not changed.
 *
 * \return 0 on success or an error code otherwise
 * \retval -EINVAL One of the control is not supported or not accessible
 * \retval i The index of the control that failed
 */
int V4L2Device::setControls(ControlList *ctrls)
{
	if (ctrls->empty())
		return 0;

	std::vector<v4l2_ext_control> v4l2Ctrls(ctrls->size());
	memset(v4l2Ctrls.data(), 0, sizeof(v4l2_ext_control) * ctrls->size());

	for (auto [ctrl, i] = std::pair(ctrls->begin(), 0u); i < ctrls->size(); ctrl++, i++) {
		const unsigned int id = ctrl->first;
		const auto iter = controls_.find(id);
		if (iter == controls_.end()) {
			LOG(V4L2, Error)
				<< "Control " << utils::hex(id) << " not found";
			return -EINVAL;
		}
		v4l2_ext_control &v4l2Ctrl = v4l2Ctrls[i];
		v4l2Ctrl.id = id;

		/* Set the v4l2_ext_control value for the write operation. */
		ControlValue &value = ctrl->second;
		switch (iter->first->type()) {
		case ControlTypeInteger64:
			v4l2Ctrl.value64 = value.get<int64_t>();
			break;

		case ControlTypeByte: {
			if (!value.isArray()) {
				LOG(V4L2, Error)
					<< "Control " << utils::hex(id)
					<< " requires an array value";
				return -EINVAL;
			}

			Span<uint8_t> data = value.data();
			v4l2Ctrl.p_u8 = data.data();
			v4l2Ctrl.size = data.size();

			break;
		}

		default:
			/* \todo To be changed to support strings. */
			v4l2Ctrl.value = value.get<int32_t>();
			break;
		}
	}

	struct v4l2_ext_controls v4l2ExtCtrls = {};
	v4l2ExtCtrls.which = V4L2_CTRL_WHICH_CUR_VAL;
	v4l2ExtCtrls.controls = v4l2Ctrls.data();
	v4l2ExtCtrls.count = v4l2Ctrls.size();

	int ret = ioctl(VIDIOC_S_EXT_CTRLS, &v4l2ExtCtrls);
	if (ret) {
		unsigned int errorIdx = v4l2ExtCtrls.error_idx;

		/* Generic validation error. */
		if (errorIdx == 0 || errorIdx >= v4l2Ctrls.size()) {
			LOG(V4L2, Error) << "Unable to set controls: "
					 << strerror(-ret);
			return -EINVAL;
		}

		/* A specific control failed. */
		LOG(V4L2, Error) << "Unable to set control " << errorIdx
				 << ": " << strerror(-ret);

		v4l2Ctrls.resize(errorIdx);
		ret = errorIdx;
	}

	updateControls(ctrls, v4l2Ctrls);

	return ret;
}

/**
 * \brief Retrieve the v4l2_query_ext_ctrl information for the given control
 * \param[in] id The V4L2 control id
 * \return A pointer to the v4l2_query_ext_ctrl structure for the given
 * control, or a null pointer if not found
 */
const struct v4l2_query_ext_ctrl *V4L2Device::controlInfo(uint32_t id) const
{
	const auto it = controlInfo_.find(id);
	if (it == controlInfo_.end())
		return nullptr;

	return &it->second;
}

/**
 * \brief Retrieve the device path in sysfs
 *
 * This function returns the sysfs path to the physical device backing the V4L2
 * device. The path is guaranteed to be an absolute path, without any symbolic
 * link.
 *
 * It includes the sysfs mount point prefix
 *
 * \return The device path in sysfs
 */
std::string V4L2Device::devicePath() const
{
	std::string devicePath = sysfs::charDevPath(deviceNode_) + "/device";

	char *realPath = realpath(devicePath.c_str(), nullptr);
	if (!realPath) {
		LOG(V4L2, Fatal)
			<< "Can not resolve device path for " << devicePath;
		return {};
	}

	std::string path{ realPath };
	free(realPath);

	return path;
}

/**
 * \brief Enable or disable frame start event notification
 * \param[in] enable True to enable frame start events, false to disable them
 *
 * This function enables or disables generation of frame start events. Once
 * enabled, the events are signalled through the frameStart signal.
 *
 * \return 0 on success, a negative error code otherwise
 */
int V4L2Device::setFrameStartEnabled(bool enable)
{
	if (frameStartEnabled_ == enable)
		return 0;

	struct v4l2_event_subscription event{};
	event.type = V4L2_EVENT_FRAME_SYNC;

	unsigned long request = enable ? VIDIOC_SUBSCRIBE_EVENT
			      : VIDIOC_UNSUBSCRIBE_EVENT;
	int ret = ioctl(request, &event);
	if (enable && ret)
		return ret;

	fdEventNotifier_->setEnabled(enable);
	frameStartEnabled_ = enable;

	return ret;
}

/**
 * \var V4L2Device::frameStart
 * \brief A Signal emitted when capture of a frame has started
 */

/**
 * \brief Perform an IOCTL system call on the device node
 * \param[in] request The IOCTL request code
 * \param[in] argp A pointer to the IOCTL argument
 * \return 0 on success or a negative error code otherwise
 */
int V4L2Device::ioctl(unsigned long request, void *argp)
{
	/*
	 * Printing out an error message is usually better performed
	 * in the caller, which can provide more context.
	 */
	if (::ioctl(fd_, request, argp) < 0)
		return -errno;

	return 0;
}

/**
 * \fn V4L2Device::deviceNode()
 * \brief Retrieve the device node path
 * \return The device node path
 */

/**
 * \fn V4L2Device::fd()
 * \brief Retrieve the V4L2 device file descriptor
 * \return The V4L2 device file descriptor, -1 if the device node is not open
 */

/*
 * \brief List and store information about all controls supported by the
 * V4L2 device
 */
void V4L2Device::listControls()
{
	ControlInfoMap::Map ctrls;
	struct v4l2_query_ext_ctrl ctrl = {};

	/* \todo Add support for menu controls. */
	while (1) {
		ctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL |
			   V4L2_CTRL_FLAG_NEXT_COMPOUND;
		if (ioctl(VIDIOC_QUERY_EXT_CTRL, &ctrl))
			break;

		if (ctrl.type == V4L2_CTRL_TYPE_CTRL_CLASS ||
		    ctrl.flags & V4L2_CTRL_FLAG_DISABLED)
			continue;

		switch (ctrl.type) {
		case V4L2_CTRL_TYPE_INTEGER:
		case V4L2_CTRL_TYPE_BOOLEAN:
		case V4L2_CTRL_TYPE_MENU:
		case V4L2_CTRL_TYPE_BUTTON:
		case V4L2_CTRL_TYPE_INTEGER64:
		case V4L2_CTRL_TYPE_BITMASK:
		case V4L2_CTRL_TYPE_INTEGER_MENU:
		case V4L2_CTRL_TYPE_U8:
			break;
		/* \todo Support other control types. */
		default:
			LOG(V4L2, Debug)
				<< "Control " << utils::hex(ctrl.id)
				<< " has unsupported type " << ctrl.type;
			continue;
		}

		controlIds_.emplace_back(v4l2ControlId(ctrl));
		controlInfo_.emplace(ctrl.id, ctrl);

		ctrls.emplace(controlIds_.back().get(), v4l2ControlInfo(ctrl));
	}

	controls_ = std::move(ctrls);
}

/**
* \brief Update the information for all device controls
 *
 * The V4L2Device class caches information about all controls supported by the
 * device and exposes it through the controls() and controlInfo() functions.
 * Control information may change at runtime, for instance when formats on a
 * subdev are modified. When this occurs, this function can be used to refresh
 * control information. The information is refreshed in-place, all pointers to
 * v4l2_query_ext_ctrl instances previously returned by controlInfo() and
 * iterators to the ControlInfoMap returned by controls() remain valid.
 *
 * Note that control information isn't refreshed automatically is it may be an
 * expensive operation. The V4L2Device users are responsible for calling this
 * function when required, based on their usage pattern of the class.
 */
void V4L2Device::updateControlInfo()
{
	for (auto &[controlId, info] : controls_) {
		unsigned int id = controlId->id();

		/*
		 * Assume controlInfo_ has a corresponding entry, as it has been
		 * generated by listControls().
		 */
		struct v4l2_query_ext_ctrl &ctrl = controlInfo_[id];

		if (ioctl(VIDIOC_QUERY_EXT_CTRL, &ctrl)) {
			LOG(V4L2, Debug)
				<< "Could not refresh control "
				<< utils::hex(id);
			continue;
		}

		info = v4l2ControlInfo(ctrl);
	}
}

/*
 * \brief Update the value of the first \a count V4L2 controls in \a ctrls using
 * values in \a v4l2Ctrls
 * \param[inout] ctrls List of V4L2 controls to update
 * \param[in] v4l2Ctrls List of V4L2 extended controls as returned by the driver
 */
void V4L2Device::updateControls(ControlList *ctrls,
				Span<const v4l2_ext_control> v4l2Ctrls)
{
	for (const v4l2_ext_control &v4l2Ctrl : v4l2Ctrls) {
		const unsigned int id = v4l2Ctrl.id;

		ControlValue value = ctrls->get(id);

		const auto iter = controls_.find(id);
		ASSERT(iter != controls_.end());

		switch (iter->first->type()) {
		case ControlTypeInteger64:
			value.set<int64_t>(v4l2Ctrl.value64);
			break;

		case ControlTypeByte:
			/*
			 * No action required, the VIDIOC_[GS]_EXT_CTRLS ioctl
			 * accessed the ControlValue storage directly.
			 */
			break;

		default:
			/*
			 * \todo To be changed when support for string controls
			 * will be added.
			 */
			value.set<int32_t>(v4l2Ctrl.value);
			break;
		}

		ctrls->set(id, value);
	}
}

/**
 * \brief Slot to handle V4L2 events from the V4L2 device
 * \param[in] notifier The event notifier
 *
 * When this slot is called, a V4L2 event is available to be dequeued from the
 * device.
 */
void V4L2Device::eventAvailable([[maybe_unused]] EventNotifier *notifier)
{
	struct v4l2_event event{};
	int ret = ioctl(VIDIOC_DQEVENT, &event);
	if (ret < 0) {
		LOG(V4L2, Error)
			<< "Failed to dequeue event, disabling event notifier";
		fdEventNotifier_->setEnabled(false);
		return;
	}

	if (event.type != V4L2_EVENT_FRAME_SYNC) {
		LOG(V4L2, Error)
			<< "Spurious event (" << event.type
			<< "), disabling event notifier";
		fdEventNotifier_->setEnabled(false);
		return;
	}

	frameStart.emit(event.u.frame_sync.frame_sequence);
}

} /* namespace libcamera */
