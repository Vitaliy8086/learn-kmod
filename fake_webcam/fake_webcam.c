#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-vmalloc.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Nichol");
MODULE_DESCRIPTION("A virtual webcam driver.");
MODULE_VERSION("0.01");

struct fw_info {
  struct mutex ioctl_lock;

  struct video_device* dev;
  struct v4l2_device parent_dev;
  struct vb2_queue queue;

  // Tracking frame timing.
  int is_first_frame;
  u64 next_time;
};

static struct fw_info fw_info;

// File operations

static ssize_t fw_read(struct file* f,
                       char __user* data,
                       size_t size,
                       loff_t* off) {
  return vb2_read(&fw_info.queue, data, size, off, f->f_flags & O_NONBLOCK);
}

static unsigned int fw_poll(struct file* f, struct poll_table_struct* table) {
  return vb2_poll(&fw_info.queue, f, table);
}

static int fw_mmap(struct file* f, struct vm_area_struct* vma) {
  return vb2_mmap(&fw_info.queue, vma);
}

static struct v4l2_file_operations fw_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = video_ioctl2,
    .open = v4l2_fh_open,
    .release = v4l2_fh_release,  // TODO: need to release the queue here?
    .read = fw_read,
    .poll = fw_poll,
    .mmap = fw_mmap,
};

// IOCTL operations

static const char* fw_fmt_description = "4:2:2, packed, YUYV";
static const u32 fw_fmt_pixelformat = V4L2_PIX_FMT_YUYV;
static const int fw_fmt_depth = 16;
static const int fw_fmt_width = 640;
static const int fw_fmt_height = 480;
static const int fw_fmt_field = V4L2_FIELD_INTERLACED;
static const int fw_fmt_colorspace = V4L2_COLORSPACE_SMPTE170M;
static const int fw_fmt_std = V4L2_STD_525_60;

static int fw_vidioc_querycap(struct file* f,
                              void* priv,
                              struct v4l2_capability* cap) {
  strncpy(cap->driver, "fake_webcam", sizeof(cap->driver));
  strncpy(cap->card, "fake_webcam", sizeof(cap->card));
  strncpy(cap->bus_info, "fake_webcam", sizeof(cap->bus_info));
  cap->device_caps =
      V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
  cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
  return 0;
}

static int fw_vidioc_enum_fmt_vid_cap(struct file* f,
                                      void* priv,
                                      struct v4l2_fmtdesc* fmt) {
  if (fmt->index) {
    return -EINVAL;
  }
  strncpy(fmt->description, fw_fmt_description, sizeof(fmt->description));
  fmt->pixelformat = fw_fmt_pixelformat;
  return 0;
}

static int fw_vidioc_g_fmt_vid_cap(struct file* f,
                                   void* priv,
                                   struct v4l2_format* fmt) {
  fmt->fmt.pix.width = fw_fmt_width;
  fmt->fmt.pix.height = fw_fmt_height;
  fmt->fmt.pix.field = fw_fmt_field;
  fmt->fmt.pix.pixelformat = fw_fmt_pixelformat;
  fmt->fmt.pix.bytesperline = (fw_fmt_width * fw_fmt_depth) / 8;
  fmt->fmt.pix.sizeimage = fw_fmt_height * fmt->fmt.pix.bytesperline;
  fmt->fmt.pix.colorspace = fw_fmt_colorspace;
  return 0;
}

static int fw_vidioc_try_fmt_vid_cap(struct file* f,
                                     void* priv,
                                     struct v4l2_format* fmt) {
  if (fmt->fmt.pix.pixelformat != fw_fmt_pixelformat) {
    return -EINVAL;
  }
  return fw_vidioc_g_fmt_vid_cap(f, priv, fmt);
}

static int fw_vidioc_s_std(struct file* f, void* priv, v4l2_std_id id) {
  return 0;
}

static int fw_vidioc_enum_input(struct file* f,
                                void* priv,
                                struct v4l2_input* input) {
  if (input->index) {
    return -EINVAL;
  }
  input->type = V4L2_INPUT_TYPE_CAMERA;
  input->std = fw_fmt_std;
  strncpy(input->name, "Fake Webcam", sizeof(input->name));
  return 0;
}

static int fw_vidioc_g_input(struct file* f, void* priv, unsigned int* i) {
  *i = 0;
  return 0;
}

static int fw_vidioc_s_input(struct file* f, void* priv, unsigned int i) {
  if (i) {
    return -EINVAL;
  }
  return 0;
}

static int fw_vidioc_reqbufs(struct file* f,
                             void* priv,
                             struct v4l2_requestbuffers* req) {
  return vb2_reqbufs(&fw_info.queue, req);
}

static int fw_vidioc_querybuf(struct file* f,
                              void* priv,
                              struct v4l2_buffer* buffer) {
  return vb2_querybuf(&fw_info.queue, buffer);
}

static int fw_vidioc_qbuf(struct file* f,
                          void* priv,
                          struct v4l2_buffer* buffer) {
  return vb2_qbuf(&fw_info.queue, buffer);
}

static int fw_vidioc_dqbuf(struct file* f,
                           void* priv,
                           struct v4l2_buffer* buffer) {
  return vb2_dqbuf(&fw_info.queue, buffer, f->f_flags & O_NONBLOCK);
}

static int fw_vidioc_streamon(struct file* f,
                              void* priv,
                              enum v4l2_buf_type t) {
  return vb2_streamon(&fw_info.queue, t);
}

static int fw_vidioc_streamoff(struct file* f,
                               void* priv,
                               enum v4l2_buf_type t) {
  return vb2_streamoff(&fw_info.queue, t);
}

static struct v4l2_ioctl_ops fw_ioctl_ops = {
    // Capabilities and formats.
    .vidioc_querycap = fw_vidioc_querycap,
    .vidioc_enum_fmt_vid_cap = fw_vidioc_enum_fmt_vid_cap,
    .vidioc_g_fmt_vid_cap = fw_vidioc_g_fmt_vid_cap,
    .vidioc_s_fmt_vid_cap = fw_vidioc_try_fmt_vid_cap,
    .vidioc_try_fmt_vid_cap = fw_vidioc_try_fmt_vid_cap,
    .vidioc_s_std = fw_vidioc_s_std,
    .vidioc_enum_input = fw_vidioc_enum_input,
    .vidioc_g_input = fw_vidioc_g_input,
    .vidioc_s_input = fw_vidioc_s_input,

    // Buffer manipulation.
    .vidioc_reqbufs = fw_vidioc_reqbufs,
    .vidioc_querybuf = fw_vidioc_querybuf,
    .vidioc_qbuf = fw_vidioc_qbuf,
    .vidioc_dqbuf = fw_vidioc_dqbuf,
    .vidioc_streamon = fw_vidioc_streamon,
    .vidioc_streamoff = fw_vidioc_streamoff,
};

// Device operations

static void fw_video_device_release(struct video_device* dev) {
  video_device_release(dev);
  // TODO: see if this will be necessary.
  // media_entity_cleanup(&dev->entity);
}

// Video buffer operations

static int fw_vb2_queue_setup(struct vb2_queue* queue,
                              unsigned int* nbuffers,
                              unsigned int* nplanes,
                              unsigned int sizes[],
                              struct device* alloc_devs[]) {
  *nbuffers = 4;
  *nplanes = 1;
  sizes[0] = (fw_fmt_width * fw_fmt_height * fw_fmt_depth) / 8;
  return 0;
}

static void fw_vb2_wait_prepare(struct vb2_queue* queue) {
  mutex_unlock(&fw_info.ioctl_lock);
}

static void fw_vb2_wait_finish(struct vb2_queue* queue) {
  mutex_lock(&fw_info.ioctl_lock);
}

static int fw_vb2_start_streaming(struct vb2_queue* queue, unsigned int count) {
  fw_info.is_first_frame = 1;
  return 0;
}

static void fw_vb2_stop_streaming(struct vb2_queue* queue) {
  vb2_wait_for_all_buffers(queue);
}

static u64 fw_get_nanotime(void) {
  struct timeval val;
  do_gettimeofday(&val);
  return (u64)val.tv_sec * 1000000000 + (u64)val.tv_usec * 1000;
}

static int fw_ship_buffer_thread(void* buf_void) {
  struct vb2_buffer* buffer = (struct vb2_buffer*)buf_void;

  u64 nanotime = fw_get_nanotime();
  if (nanotime < buffer->timestamp) {
    schedule_timeout_interruptible(
        nsecs_to_jiffies64(buffer->timestamp - nanotime));
  }

  vb2_buffer_done(buffer, VB2_BUF_STATE_DONE);

  do_exit(0);
  return 0;
}

static void fw_vb2_buf_queue(struct vb2_buffer* buffer) {
  u64 nanotime = fw_get_nanotime();
  if (fw_info.is_first_frame) {
    fw_info.is_first_frame = 0;
    fw_info.next_time = nanotime;
  } else if (fw_info.next_time < nanotime) {
    // The frames aren't being consumed fast enough.
    fw_info.next_time = nanotime;
  }
  buffer->timestamp = fw_info.next_time;
  kthread_run(fw_ship_buffer_thread, buffer, "bufqueue");
}

static struct vb2_ops fw_vb2_ops = {
    .queue_setup = fw_vb2_queue_setup,
    .wait_prepare = fw_vb2_wait_prepare,
    .wait_finish = fw_vb2_wait_finish,
    .start_streaming = fw_vb2_start_streaming,
    .stop_streaming = fw_vb2_stop_streaming,
    .buf_queue = fw_vb2_buf_queue,
};

// Module lifecycle

static int __init fw_init(void) {
  int res;

  memset(&fw_info, 0, sizeof(fw_info));
  mutex_init(&fw_info.ioctl_lock);

  strncpy(fw_info.parent_dev.name, "fake_webcam",
          sizeof(fw_info.parent_dev.name));
  res = v4l2_device_register(NULL, &fw_info.parent_dev);
  if (res) {
    goto fail;
  }

  fw_info.queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fw_info.queue.io_modes = VB2_MMAP | VB2_USERPTR | VB2_READ;
  fw_info.queue.dev = fw_info.parent_dev.dev;
  fw_info.queue.ops = &fw_vb2_ops;
  fw_info.queue.mem_ops = &vb2_vmalloc_memops;
  fw_info.queue.buf_struct_size = sizeof(struct vb2_buffer);
  vb2_queue_init(&fw_info.queue);

  fw_info.dev = video_device_alloc();
  if (!fw_info.dev) {
    res = -ENOMEM;
    goto fail_unregister;
  }
  fw_info.dev->release = fw_video_device_release;
  fw_info.dev->v4l2_dev = &fw_info.parent_dev;
  strncpy(fw_info.dev->name, "Fake Webcam", sizeof(fw_info.dev->name));
  fw_info.dev->vfl_dir = VFL_DIR_RX;
  fw_info.dev->fops = &fw_fops;
  fw_info.dev->ioctl_ops = &fw_ioctl_ops;
  fw_info.dev->lock = &fw_info.ioctl_lock;
  fw_info.dev->queue = &fw_info.queue;

  res = video_register_device_no_warn(fw_info.dev, VFL_TYPE_GRABBER, -1);
  if (res < 0) {
    goto fail_device_free;
  }

  return 0;

fail_device_free:
  video_device_release(fw_info.dev);
fail_unregister:
  v4l2_device_unregister(&fw_info.parent_dev);
  vb2_queue_release(&fw_info.queue);
fail:
  return res;
}

static void __exit fw_exit(void) {
  video_unregister_device(fw_info.dev);
  v4l2_device_unregister(&fw_info.parent_dev);
  vb2_queue_release(&fw_info.queue);
}

module_init(fw_init);
module_exit(fw_exit);