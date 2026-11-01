/*
 * gxfpnav: Goodix GXFP51B7 (MilanL) over the EC mailbox, with images read through
 * cleartext NAV frames. It uses neither TLS nor a PSK, so the channel is not
 * authenticated. Images are read in a thread because a sweep blocks for about 2 s.
 * Finger detection polls NAV: the Windows FDT flow has not been tested without TLS.
 *
 * Copyright (C) 2026 Miguel Almont
 *
 * This library is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free Software
 * Foundation; either version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU Lesser General Public License for more details.
 */
#define FP_COMPONENT "gxfpnav"

#include "drivers_api.h"

#include "../fpi-image-device.h"

#include "gxfpnav_core.h"

#include <errno.h>
#include <gio/gio.h>
#include <string.h>

#define GXFPNAV_ENROLL_SAMPLES 10
#define GXFPNAV_DATA_DIR "/var/lib/fprint/gxfpnav"
#define GXFPNAV_POLL_US 100000
#define GXFPNAV_FINGER_DOWN (-200.0)	/* mean difference: about -500 counts with a finger */
#define GXFPNAV_FINGER_UP (-80.0)
#define GXFPNAV_DOWN_STABLE 2

enum gxfpnav_op {
  GXFPNAV_OP_NONE,
  GXFPNAV_OP_WAIT_DOWN,
  GXFPNAV_OP_WAIT_UP,
  GXFPNAV_OP_CAPTURE,
};

struct _FpiDeviceGxfpNav {
  FpImageDevice parent;

  struct gxfpnav_core core;
  uint8_t image[GXFPNAV_IMG_W * GXFPNAV_IMG_H];

  GCancellable *cancellable;
  gboolean op_running;
  gboolean deactivating;

  /* Suspend: the operation stops before sleeping and is repeated on wake-up. */
  gboolean suspending;
  gboolean suspended;
  gboolean reinit;
  enum gxfpnav_op pending_op;
};

G_DECLARE_FINAL_TYPE (FpiDeviceGxfpNav, fpi_device_gxfpnav, FPI, DEVICE_GXFPNAV, FpImageDevice)
G_DEFINE_TYPE (FpiDeviceGxfpNav, fpi_device_gxfpnav, FP_TYPE_IMAGE_DEVICE)

static int
gxfpnav_is_cancelled (void *ctx)
{
  return g_cancellable_is_cancelled (G_CANCELLABLE (ctx));
}

static int
gxfpnav_wait_finger (FpiDeviceGxfpNav *self, GCancellable *cancellable, gboolean down)
{
  int stable = 0;

  while (!g_cancellable_is_cancelled (cancellable))
    {
      double delta = 0;
      int r = gxfpnav_finger_delta (&self->core, &delta);

      /* A damaged frame says nothing about the finger. */
      if (r == -EAGAIN)
        continue;
      if (r < 0)
        return r;

      if (down ? delta < GXFPNAV_FINGER_DOWN : delta > GXFPNAV_FINGER_UP)
        stable++;
      else
        stable = 0;
      if (stable >= (down ? GXFPNAV_DOWN_STABLE : 1))
        return 0;

      g_usleep (GXFPNAV_POLL_US);
    }
  return -ECANCELED;
}

static void
gxfpnav_op_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  FpiDeviceGxfpNav *self = FPI_DEVICE_GXFPNAV (source);
  enum gxfpnav_op op = GPOINTER_TO_INT (task_data);
  double overlap = 0;
  int r;

  /* The EC state after S3 is unknown: repeat the sequence already proven at open. */
  if (self->reinit)
    {
      self->reinit = FALSE;
      r = gxfpnav_sensor_init (&self->core);
      fp_dbg ("reinit after suspend: r=%d", r);
      if (r < 0)
        {
          g_task_return_int (task, r);
          return;
        }
    }

  switch (op)
    {
    case GXFPNAV_OP_WAIT_DOWN:
      r = gxfpnav_wait_finger (self, cancellable, TRUE);
      break;

    case GXFPNAV_OP_WAIT_UP:
      r = gxfpnav_wait_finger (self, cancellable, FALSE);
      break;

    case GXFPNAV_OP_CAPTURE:
      r = gxfpnav_capture (&self->core, self->image, &overlap, gxfpnav_is_cancelled, cancellable);
      fp_dbg ("capture: r=%d, overlap=%.3f", r, overlap);
      break;

    case GXFPNAV_OP_NONE:
    default:
      g_assert_not_reached ();
    }
  g_task_return_int (task, r);
}

static void
gxfpnav_suspend_done (FpiDeviceGxfpNav *self)
{
  self->suspending = FALSE;
  self->suspended = TRUE;
  fpi_device_suspend_complete (FP_DEVICE (self), NULL);
}

static void
gxfpnav_op_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  FpiDeviceGxfpNav *self = FPI_DEVICE_GXFPNAV (source);
  FpImageDevice *dev = FP_IMAGE_DEVICE (self);
  enum gxfpnav_op op = GPOINTER_TO_INT (user_data);
  int r = (int) g_task_propagate_int (G_TASK (result), NULL);

  self->op_running = FALSE;

  if (self->deactivating)
    {
      fpi_image_device_deactivate_complete (dev, NULL);
      if (self->suspending)
        gxfpnav_suspend_done (self);
      return;
    }

  if (self->suspending)
    {
      /* An operation requested while this one was being cancelled is more recent. */
      if (self->pending_op == GXFPNAV_OP_NONE)
        self->pending_op = op;
      gxfpnav_suspend_done (self);
      return;
    }

  if (r == -EAGAIN && op == GXFPNAV_OP_CAPTURE)
    {
      fpi_image_device_retry_scan (dev, FP_DEVICE_RETRY_CENTER_FINGER);
      return;
    }
  if (r < 0)
    {
      fpi_image_device_session_error (dev,
          g_error_new (G_IO_ERROR, g_io_error_from_errno (-r),
                       "gxfpnav: %s", g_strerror (-r)));
      return;
    }

  switch (op)
    {
    case GXFPNAV_OP_WAIT_DOWN:
      fpi_image_device_report_finger_status (dev, TRUE);
      break;

    case GXFPNAV_OP_WAIT_UP:
      fpi_image_device_report_finger_status (dev, FALSE);
      break;

    case GXFPNAV_OP_CAPTURE:
      {
        FpImage *img = fp_image_new (GXFPNAV_IMG_W, GXFPNAV_IMG_H);

        memcpy (img->data, self->image, sizeof (self->image));
        fpi_image_device_image_captured (dev, img);
      }
      break;

    case GXFPNAV_OP_NONE:
    default:
      g_assert_not_reached ();
    }
}

static void
gxfpnav_start_op (FpiDeviceGxfpNav *self, enum gxfpnav_op op)
{
  g_autoptr(GTask) task = NULL;

  if (self->suspending || self->suspended)
    {
      self->pending_op = op;
      return;
    }

  g_return_if_fail (!self->op_running);

  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();
  self->op_running = TRUE;

  task = g_task_new (self, self->cancellable, gxfpnav_op_done, GINT_TO_POINTER (op));
  g_task_set_task_data (task, GINT_TO_POINTER (op), NULL);
  g_task_run_in_thread (task, gxfpnav_op_thread);
}

static void
gxfpnav_img_open (FpImageDevice *dev)
{
  FpiDeviceGxfpNav *self = FPI_DEVICE_GXFPNAV (dev);
  const gchar *chardev = fpi_device_get_udev_data (FP_DEVICE (self),
                                                   FPI_DEVICE_UDEV_SUBTYPE_CHARDEV);
  const gchar *dir = g_getenv ("FP_GXFPNAV_DIR");
  g_autofree gchar *cfg = NULL;
  g_autofree gchar *ref = NULL;
  int r;

  if (!dir || !dir[0])
    dir = GXFPNAV_DATA_DIR;
  cfg = g_build_filename (dir, "config.bin", NULL);
  ref = g_build_filename (dir, "reference.bin", NULL);

  r = gxfpnav_open (&self->core, chardev, cfg, ref);
  if (r < 0)
    {
      fpi_image_device_open_complete (dev,
          g_error_new (G_IO_ERROR, g_io_error_from_errno (-r),
                       "gxfpnav: open with data from %s: %s", dir, g_strerror (-r)));
      return;
    }
  fpi_image_device_open_complete (dev, NULL);
}

static void
gxfpnav_img_close (FpImageDevice *dev)
{
  gxfpnav_close (&FPI_DEVICE_GXFPNAV (dev)->core);
  fpi_image_device_close_complete (dev, NULL);
}

static void
gxfpnav_activate (FpImageDevice *dev)
{
  FpiDeviceGxfpNav *self = FPI_DEVICE_GXFPNAV (dev);

  /* If the action ended while asleep, libfprint does not call resume. */
  self->deactivating = FALSE;
  self->suspended = FALSE;
  self->pending_op = GXFPNAV_OP_NONE;
  fpi_image_device_activate_complete (dev, NULL);
}

static void
gxfpnav_deactivate (FpImageDevice *dev)
{
  FpiDeviceGxfpNav *self = FPI_DEVICE_GXFPNAV (dev);

  self->deactivating = TRUE;
  self->pending_op = GXFPNAV_OP_NONE;
  if (self->op_running)
    g_cancellable_cancel (self->cancellable);
  else
    fpi_image_device_deactivate_complete (dev, NULL);
}

static void
gxfpnav_change_state (FpImageDevice *dev, FpiImageDeviceState state)
{
  FpiDeviceGxfpNav *self = FPI_DEVICE_GXFPNAV (dev);

  switch (state)
    {
    case FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON:
      gxfpnav_start_op (self, GXFPNAV_OP_WAIT_DOWN);
      break;

    case FPI_IMAGE_DEVICE_STATE_CAPTURE:
      gxfpnav_start_op (self, GXFPNAV_OP_CAPTURE);
      break;

    case FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF:
      gxfpnav_start_op (self, GXFPNAV_OP_WAIT_UP);
      break;

    case FPI_IMAGE_DEVICE_STATE_INACTIVE:
    case FPI_IMAGE_DEVICE_STATE_DEACTIVATING:
    case FPI_IMAGE_DEVICE_STATE_ACTIVATING:
    case FPI_IMAGE_DEVICE_STATE_IDLE:
      break;
    }
}

static void
gxfpnav_suspend (FpDevice *device)
{
  FpiDeviceGxfpNav *self = FPI_DEVICE_GXFPNAV (device);

  if (self->op_running)
    {
      self->suspending = TRUE;
      g_cancellable_cancel (self->cancellable);
    }
  else
    {
      gxfpnav_suspend_done (self);
    }
}

static void
gxfpnav_resume (FpDevice *device)
{
  FpiDeviceGxfpNav *self = FPI_DEVICE_GXFPNAV (device);
  enum gxfpnav_op op = self->pending_op;

  self->suspended = FALSE;
  self->pending_op = GXFPNAV_OP_NONE;
  fpi_device_resume_complete (device, NULL);

  if (op != GXFPNAV_OP_NONE && !self->deactivating)
    {
      self->reinit = TRUE;
      gxfpnav_start_op (self, op);
    }
}

static const FpIdEntry driver_ids[] = {
  { .udev_types = FPI_DEVICE_UDEV_SUBTYPE_CHARDEV, .chardev_acpi_hid = "GXFP51B7" },
  { .udev_types = 0 },
};

static void
fpi_device_gxfpnav_dispose (GObject *object)
{
  FpiDeviceGxfpNav *self = FPI_DEVICE_GXFPNAV (object);

  g_clear_object (&self->cancellable);
  G_OBJECT_CLASS (fpi_device_gxfpnav_parent_class)->dispose (object);
}

static void
fpi_device_gxfpnav_init (FpiDeviceGxfpNav *self)
{
}

static void
fpi_device_gxfpnav_class_init (FpiDeviceGxfpNavClass *klass)
{
  GObjectClass *obj_class = G_OBJECT_CLASS (klass);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  obj_class->dispose = fpi_device_gxfpnav_dispose;

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "Goodix GXFP51B7 (NAV, no TLS)";
  dev_class->type = FP_DEVICE_TYPE_UDEV;
  dev_class->id_table = driver_ids;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = GXFPNAV_ENROLL_SAMPLES;
  dev_class->temp_hot_seconds = -1;
  dev_class->suspend = gxfpnav_suspend;
  dev_class->resume = gxfpnav_resume;

  img_class->img_width = GXFPNAV_IMG_W;
  img_class->img_height = GXFPNAV_IMG_H;
  img_class->algorithm = FPI_DEVICE_ALGO_SIGFM;

  img_class->img_open = gxfpnav_img_open;
  img_class->img_close = gxfpnav_img_close;
  img_class->activate = gxfpnav_activate;
  img_class->deactivate = gxfpnav_deactivate;
  img_class->change_state = gxfpnav_change_state;
}
