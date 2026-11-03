#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/resource.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-util.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/util/log.h>
#include <xf86drm.h>
#include "render/drm_syncobj_merger.h"
#include "util/time.h"

#include "config.h"

#if HAVE_LINUX_SYNC_FILE

#include <linux/sync_file.h>
#include <sys/ioctl.h>

static int sync_file_merge(int fd1, int fd2) {
	// The kernel will automatically prune signalled fences
	struct sync_merge_data merge_data = { .fd2 = fd2 };
	if (ioctl(fd1, SYNC_IOC_MERGE, &merge_data) < 0) {
		int error = errno;
		wlr_log_errno(WLR_ERROR, "ioctl(SYNC_IOC_MERGE) failed");
		errno = error;
		return -1;
	}

	return merge_data.fence;
}

#else

static int sync_file_merge(int fd1, int fd2) {
	wlr_log(WLR_ERROR, "sync_file support is unavailable");
	errno = ENOSYS;
	return -1;
}

#endif

struct wlr_drm_syncobj_merger *wlr_drm_syncobj_merger_create(
		struct wlr_drm_syncobj_timeline *dst_timeline, uint64_t dst_point) {
	struct wlr_drm_syncobj_merger *merger = calloc(1, sizeof(*merger));
	if (merger == NULL) {
		return NULL;
	}
	merger->n_ref = 1;
	merger->dst_timeline = wlr_drm_syncobj_timeline_ref(dst_timeline);
	merger->dst_point = dst_point;
	merger->sync_fd = -1;
	return merger;
}

struct wlr_drm_syncobj_merger *wlr_drm_syncobj_merger_ref(
		struct wlr_drm_syncobj_merger *merger) {
	assert(merger->n_ref > 0);
	merger->n_ref++;
	return merger;
}

// Error-path diagnostics must work even when EMFILE prevents opening /proc.
// Count without allocating a descriptor, with a bounded scan for huge limits.
static void merger_log_failure(struct wlr_drm_syncobj_merger *merger,
		struct wlr_drm_syncobj_timeline *timeline, uint64_t point,
		const char *operation, int error) {
	int saved_errno = errno;
	merger->recovery_count++;
	struct rlimit limit = {0};
	bool have_limit = getrlimit(RLIMIT_NOFILE, &limit) == 0;
	rlim_t scan = have_limit && limit.rlim_cur < 65536 ? limit.rlim_cur : 65536;
	unsigned count = 0;
	for (rlim_t fd = 0; fd < scan; fd++) {
		if (fcntl((int)fd, F_GETFD) >= 0) {
			count++;
		}
	}
	char path[64], device[256] = "unknown";
	snprintf(path, sizeof(path), "/proc/self/fd/%d", timeline->drm_fd);
	ssize_t n = readlink(path, device, sizeof(device) - 1);
	if (n >= 0) {
		device[n] = '\0';
	}
	wlr_log(WLR_ERROR, "Release dependency failure: operation=%s error=%d "
		"device=%s drm_fd=%d handle=%"PRIu32" point=%"PRIu64" "
		"destination_fd=%d destination_point=%"PRIu64" "
		"open_fds=%u%s fd_scan_limit=%ju soft_limit=%ju hard_limit=%ju",
		operation, error, device, timeline->drm_fd, timeline->handle, point,
		merger->dst_timeline->drm_fd, merger->dst_point, count,
		!have_limit || scan < limit.rlim_cur ? "+" : "", (uintmax_t)scan,
		(uintmax_t)limit.rlim_cur, (uintmax_t)limit.rlim_max);
	errno = saved_errno;
}

// Recovery is only used after a failed synchronization operation. Never wait
// for mere availability here: the omitted dependency must actually complete.
static bool merger_wait_point(struct wlr_drm_syncobj_merger *merger,
		struct wlr_drm_syncobj_timeline *timeline, uint64_t point) {
	int64_t deadline = get_current_time_msec() + 1000;
	int ret;
	do {
		ret = drmSyncobjTimelineWait(timeline->drm_fd, &timeline->handle, &point,
			1, deadline * 1000000, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL);
	} while (ret == -EINTR && get_current_time_msec() < deadline);
	if (ret != 0) {
		merger_log_failure(merger, timeline, point, "completion-wait", -ret);
		wlr_log(WLR_ERROR, "Cannot establish release dependency completion (1 second limit): %d", ret);
		merger->failed = true;
		return false;
	}
	return true;
}

static bool merger_wait_sync_file(int fd) {
	int64_t deadline = get_current_time_msec() + 1000;
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int timeout = 1000, ret;
	do {
		ret = poll(&pfd, 1, timeout);
		timeout = deadline - get_current_time_msec();
	} while (ret < 0 && errno == EINTR && timeout > 0);
	return ret > 0 && (pfd.revents & POLLIN) &&
		!(pfd.revents & (POLLERR | POLLHUP | POLLNVAL));
}

void wlr_drm_syncobj_merger_unref(struct wlr_drm_syncobj_merger *merger) {
	if (merger == NULL) {
		return;
	}
	assert(merger->n_ref > 0);
	merger->n_ref--;
	if (merger->n_ref > 0) {
		return;
	}

	// A caller ignoring add() failure must not turn a missing fence into an
	// unconditional signal. Buffer locks alone do not prevent client reuse.
	if (merger->failed) {
		wlr_log(WLR_ERROR, "Unresolved release dependency; terminating before unsafe buffer reuse");
		abort();
	}
	bool published;
	if (merger->sync_fd >= 0) {
		published = wlr_drm_syncobj_timeline_import_sync_file(merger->dst_timeline,
			merger->dst_point, merger->sync_fd);
		if (!published) {
			merger_log_failure(merger, merger->dst_timeline, merger->dst_point, "import-release", errno);
		}
		if (!published && merger_wait_sync_file(merger->sync_fd)) {
			published = wlr_drm_syncobj_timeline_signal(merger->dst_timeline, merger->dst_point);
		}
		close(merger->sync_fd);
	} else {
		published = wlr_drm_syncobj_timeline_signal(merger->dst_timeline, merger->dst_point);
	}
	if (!published) {
		merger_log_failure(merger, merger->dst_timeline, merger->dst_point, "publish-release", errno);
		wlr_log(WLR_ERROR, "Cannot publish buffer release after bounded recovery; terminating");
		abort();
	}
	wlr_drm_syncobj_timeline_unref(merger->dst_timeline);
	free(merger);
}

static bool merger_add_exportable(struct wlr_drm_syncobj_merger *merger,
		struct wlr_drm_syncobj_timeline *src_timeline, uint64_t src_point) {
	int new_sync = wlr_drm_syncobj_timeline_export_sync_file(src_timeline, src_point);
	if (new_sync < 0) {
		merger_log_failure(merger, src_timeline, src_point, "export-sync-file", errno);
		return merger_wait_point(merger, src_timeline, src_point);
	}
	if (merger->sync_fd >= 0) {
		int merged = sync_file_merge(merger->sync_fd, new_sync);
		int merge_errno = errno;
		close(new_sync);
		if (merged < 0) {
			merger_log_failure(merger, src_timeline, src_point, "merge-sync-file", merge_errno);
			// Preserve all earlier inputs. Only this new input may be omitted,
			// and only once it is known to have completed.
			return merger_wait_point(merger, src_timeline, src_point);
		}
		close(merger->sync_fd);
		new_sync = merged;
	}
	merger->sync_fd = new_sync;
	return true;
}

struct export_waiter {
	struct wlr_drm_syncobj_timeline_waiter waiter;
	struct wlr_drm_syncobj_merger *merger;
	struct wlr_drm_syncobj_timeline *src_timeline;
	uint64_t src_point;
};

static void export_waiter_handle_ready(struct wlr_drm_syncobj_timeline_waiter *waiter) {
	struct export_waiter *add = wl_container_of(waiter, add, waiter);
	struct wlr_drm_syncobj_merger *merger = add->merger;
	bool ok = !merger->failed && merger_add_exportable(merger, add->src_timeline, add->src_point);
	wlr_drm_syncobj_timeline_unref(add->src_timeline);
	wlr_drm_syncobj_timeline_waiter_finish(&add->waiter);
	free(add);
	wlr_drm_syncobj_merger_unref(merger);
	if (!ok) {
		// There is no synchronous caller to receive this error. Do not keep
		// dispatching while a client release dependency has been lost.
		wlr_log(WLR_ERROR, "Deferred release dependency recovery failed; terminating");
		abort();
	}
}

bool wlr_drm_syncobj_merger_add(struct wlr_drm_syncobj_merger *merger,
		struct wlr_drm_syncobj_timeline *src_timeline, uint64_t src_point,
		struct wl_event_loop *loop) {
	assert(loop != NULL);
	if (merger->failed) {
		return false;
	}
	bool exportable = false;
	int flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
	if (!wlr_drm_syncobj_timeline_check(src_timeline, src_point, flags, &exportable)) {
		merger_log_failure(merger, src_timeline, src_point, "check-availability", errno);
		return merger_wait_point(merger, src_timeline, src_point);
	}
	if (exportable) {
		return merger_add_exportable(merger, src_timeline, src_point);
	}
	struct export_waiter *add = calloc(1, sizeof(*add));
	if (add == NULL) {
		merger_log_failure(merger, src_timeline, src_point, "allocate-waiter", ENOMEM);
		return merger_wait_point(merger, src_timeline, src_point);
	}
	if (!wlr_drm_syncobj_timeline_waiter_init(&add->waiter, src_timeline, src_point,
			flags, loop, export_waiter_handle_ready)) {
		free(add);
		merger_log_failure(merger, src_timeline, src_point, "initialize-waiter", errno);
		return merger_wait_point(merger, src_timeline, src_point);
	}
	add->merger = merger;
	add->src_timeline = wlr_drm_syncobj_timeline_ref(src_timeline);
	add->src_point = src_point;
	merger->n_ref++;
	return true;
}
