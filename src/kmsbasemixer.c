/*
 * (C) Copyright 2013 Kurento (http://kurento.org/)
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "kmsbasemixer.h"
#include "kmsagnosticcaps.h"
#include "kms-marshal.h"
#include "kmsmixerendpoint.h"

#define PLUGIN_NAME "basemixer"

#define KMS_BASE_MIXER_LOCK(mixer) \
  (g_rec_mutex_lock (&(mixer)->priv->mutex))

#define KMS_BASE_MIXER_UNLOCK(mixer) \
  (g_rec_mutex_unlock (&(mixer)->priv->mutex))

GST_DEBUG_CATEGORY_STATIC (kms_base_mixer_debug_category);
#define GST_CAT_DEFAULT kms_base_mixer_debug_category

#define KMS_BASE_MIXER_GET_PRIVATE(obj) (       \
  G_TYPE_INSTANCE_GET_PRIVATE (                 \
    (obj),                                      \
    KMS_TYPE_BASE_MIXER,                        \
    KmsBaseMixerPrivate                         \
  )                                             \
)

#define AUDIO_SINK_PAD_PREFIX "audio_sink_"
#define VIDEO_SINK_PAD_PREFIX "video_sink_"
#define AUDIO_SRC_PAD_PREFIX "audio_src_"
#define VIDEO_SRC_PAD_PREFIX "video_src_"
#define AUDIO_SINK_PAD_NAME AUDIO_SINK_PAD_PREFIX "%u"
#define VIDEO_SINK_PAD_NAME VIDEO_SINK_PAD_PREFIX "%u"
#define AUDIO_SRC_PAD_NAME AUDIO_SRC_PAD_PREFIX "%u"
#define VIDEO_SRC_PAD_NAME VIDEO_SRC_PAD_PREFIX "%u"

static GstStaticPadTemplate audio_sink_factory =
GST_STATIC_PAD_TEMPLATE (AUDIO_SINK_PAD_NAME,
    GST_PAD_SINK,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS (KMS_AGNOSTIC_AUDIO_CAPS)
    );

static GstStaticPadTemplate video_sink_factory =
GST_STATIC_PAD_TEMPLATE (VIDEO_SINK_PAD_NAME,
    GST_PAD_SINK,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS (KMS_AGNOSTIC_VIDEO_CAPS)
    );

static GstStaticPadTemplate audio_src_factory =
GST_STATIC_PAD_TEMPLATE (AUDIO_SRC_PAD_NAME,
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS (KMS_AGNOSTIC_AUDIO_CAPS)
    );

static GstStaticPadTemplate video_src_factory =
GST_STATIC_PAD_TEMPLATE (VIDEO_SRC_PAD_NAME,
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS (KMS_AGNOSTIC_VIDEO_CAPS)
    );

enum
{
  SIGNAL_HANDLE_PORT,
  SIGNAL_UNHANDLE_PORT,
  LAST_SIGNAL
};

static guint kms_base_mixer_signals[LAST_SIGNAL] = { 0 };

struct _KmsBaseMixerPrivate
{
  GHashTable *ports;
  GRecMutex mutex;
  gint port_count;
};

typedef struct _KmsBaseMixerPortData KmsBaseMixerPortData;

struct _KmsBaseMixerPortData
{
  KmsBaseMixer *mixer;
  GstElement *port;
  gint id;
  GstPad *audio_sink_target;
  GstPad *video_sink_target;
};

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (KmsBaseMixer, kms_base_mixer,
    GST_TYPE_BIN,
    GST_DEBUG_CATEGORY_INIT (kms_base_mixer_debug_category, PLUGIN_NAME,
        0, "debug category for basemixer element"));

static KmsBaseMixerPortData *
kms_base_mixer_port_data_create (KmsBaseMixer * mixer, GstElement * port,
    gint id)
{
  KmsBaseMixerPortData *data = g_slice_new0 (KmsBaseMixerPortData);

  data->mixer = mixer;
  data->port = g_object_ref (port);
  data->id = id;

  return data;
}

static void
kms_base_mixer_port_data_destroy (gpointer data)
{
  KmsBaseMixerPortData *port_data = (KmsBaseMixerPortData *) data;

  g_clear_object (&port_data->port);
  g_slice_free (KmsBaseMixerPortData, data);
}

gboolean
kms_base_mixer_link_video_src (KmsBaseMixer * mixer, gint id,
    GstElement * internal_element, const gchar * pad_name)
{
  g_return_val_if_fail (KMS_IS_BASE_MIXER (mixer), FALSE);

  return
      KMS_BASE_MIXER_CLASS (G_OBJECT_GET_CLASS (mixer))->link_video_src (mixer,
      id, internal_element, pad_name);
}

gboolean
kms_base_mixer_link_audio_src (KmsBaseMixer * mixer, gint id,
    GstElement * internal_element, const gchar * pad_name)
{
  g_return_val_if_fail (KMS_IS_BASE_MIXER (mixer), FALSE);

  return
      KMS_BASE_MIXER_CLASS (G_OBJECT_GET_CLASS (mixer))->link_audio_src (mixer,
      id, internal_element, pad_name);
}

gboolean
kms_base_mixer_link_video_sink (KmsBaseMixer * mixer, gint id,
    GstElement * internal_element, const gchar * pad_name)
{
  g_return_val_if_fail (KMS_IS_BASE_MIXER (mixer), FALSE);

  return
      KMS_BASE_MIXER_CLASS (G_OBJECT_GET_CLASS (mixer))->link_video_sink (mixer,
      id, internal_element, pad_name);
}

gboolean
kms_base_mixer_link_audio_sink (KmsBaseMixer * mixer, gint id,
    GstElement * internal_element, const gchar * pad_name)
{
  g_return_val_if_fail (KMS_IS_BASE_MIXER (mixer), FALSE);

  return
      KMS_BASE_MIXER_CLASS (G_OBJECT_GET_CLASS (mixer))->link_audio_sink (mixer,
      id, internal_element, pad_name);
}

static void
release_gint (gpointer data)
{
  g_slice_free (gint, data);
}

static gboolean
kms_base_mixer_link_src_pad (KmsBaseMixer * mixer, const gchar * gp_name,
    const gchar * template_name, GstElement * internal_element,
    const gchar * pad_name)
{
  GstPad *gp, *target;
  gboolean ret;

  if (GST_OBJECT_PARENT (internal_element) != GST_OBJECT (mixer)) {
    GST_ERROR_OBJECT (mixer, "Cannot link %" GST_PTR_FORMAT " wrong hierarchy",
        internal_element);
    return FALSE;
  }

  target = gst_element_get_static_pad (internal_element, pad_name);
  if (target == NULL) {
    target = gst_element_get_request_pad (internal_element, pad_name);
  }

  if (target == NULL) {
    GST_ERROR_OBJECT (mixer, "Cannot get target pad");
    return FALSE;
  }

  gp = gst_element_get_static_pad (GST_ELEMENT (mixer), gp_name);

  if (gp == NULL) {
    GstPadTemplate *templ;

    templ =
        gst_element_class_get_pad_template (GST_ELEMENT_CLASS
        (G_OBJECT_GET_CLASS (mixer)), template_name);
    gp = gst_ghost_pad_new_from_template (gp_name, target, templ);
    ret = gst_element_add_pad (GST_ELEMENT (mixer), gp);
    if (!ret) {
      g_object_unref (gp);
    }
  } else {
    ret = gst_ghost_pad_set_target (GST_GHOST_PAD (gp), target);
    g_object_unref (gp);
  }

  g_object_unref (target);

  return ret;
}

static gboolean
kms_base_mixer_link_audio_src_default (KmsBaseMixer * mixer, gint id,
    GstElement * internal_element, const gchar * pad_name)
{
  gchar *gp_name = g_strdup_printf (AUDIO_SRC_PAD_PREFIX "%d", id);
  gboolean ret;

  ret =
      kms_base_mixer_link_src_pad (mixer, gp_name, AUDIO_SRC_PAD_NAME,
      internal_element, pad_name);
  g_free (gp_name);

  return ret;
}

static gboolean
kms_base_mixer_link_video_src_default (KmsBaseMixer * mixer, gint id,
    GstElement * internal_element, const gchar * pad_name)
{
  gchar *gp_name = g_strdup_printf (VIDEO_SRC_PAD_PREFIX "%d", id);
  gboolean ret;

  ret =
      kms_base_mixer_link_src_pad (mixer, gp_name, VIDEO_SRC_PAD_NAME,
      internal_element, pad_name);
  g_free (gp_name);

  return ret;
}

static gboolean
kms_base_mixer_create_and_link_ghost_pad (KmsBaseMixer * mixer,
    GstPad * src_pad, const gchar * gp_name, const gchar * gp_template_name,
    GstPad * target)
{
  GstPadTemplate *templ;
  GstPad *gp;
  gboolean ret;

  templ =
      gst_element_class_get_pad_template (GST_ELEMENT_CLASS
      (G_OBJECT_GET_CLASS (mixer)), gp_template_name);
  gp = gst_ghost_pad_new_from_template (gp_name, target, templ);
  ret = gst_element_add_pad (GST_ELEMENT (mixer), gp);

  if (ret) {
    gst_pad_link (src_pad, gp);
  } else {
    g_object_unref (gp);
  }

  return ret;
}

static gboolean
kms_base_mixer_link_sink_pad (KmsBaseMixer * mixer, gint id,
    const gchar * gp_name, const gchar * gp_template_name,
    GstElement * internal_element, const gchar * pad_name,
    const gchar * port_src_pad_name, gulong target_offset)
{
  KmsBaseMixerPortData *port_data;
  gboolean ret;
  GstPad *gp, *target;
  GstPad **port_data_target;

  if (GST_OBJECT_PARENT (internal_element) != GST_OBJECT (mixer)) {
    GST_ERROR_OBJECT (mixer, "Cannot link %" GST_PTR_FORMAT " wrong hierarchy",
        internal_element);
    return FALSE;
  }

  target = gst_element_get_static_pad (internal_element, pad_name);
  if (target == NULL) {
    target = gst_element_get_request_pad (internal_element, pad_name);
  }

  if (target == NULL) {
    GST_ERROR_OBJECT (mixer, "Cannot get target pad");
    return FALSE;
  }

  KMS_BASE_MIXER_LOCK (mixer);

  port_data = g_hash_table_lookup (mixer->priv->ports, &id);

  if (port_data == NULL) {
    ret = FALSE;
    goto end;
  }

  port_data_target = G_STRUCT_MEMBER_P (port_data, target_offset);
  *port_data_target = g_object_ref (target);

  gp = gst_element_get_static_pad (GST_ELEMENT (mixer), gp_name);
  if (gp != NULL) {
    ret = gst_ghost_pad_set_target (GST_GHOST_PAD (gp), target);
    g_object_unref (gp);
  } else {
    GstPad *src_pad = gst_element_get_static_pad (port_data->port,
        port_src_pad_name);

    if (src_pad != NULL) {
      ret = kms_base_mixer_create_and_link_ghost_pad (mixer, src_pad,
          gp_name, gp_template_name, target);
      g_object_unref (src_pad);
    } else {
      ret = TRUE;
    }
  }

  GST_DEBUG_OBJECT (mixer, "Audio target pad for port %d: %" GST_PTR_FORMAT,
      port_data->id, port_data->audio_sink_target);
  GST_DEBUG_OBJECT (mixer, "Video target pad for port %d: %" GST_PTR_FORMAT,
      port_data->id, port_data->video_sink_target);

end:

  KMS_BASE_MIXER_UNLOCK (mixer);

  g_object_unref (target);

  return ret;
}

static gboolean
kms_base_mixer_link_video_sink_default (KmsBaseMixer * mixer, gint id,
    GstElement * internal_element, const gchar * pad_name)
{
  gboolean ret;
  gchar *gp_name = g_strdup_printf (VIDEO_SINK_PAD_PREFIX "%d", id);

  ret = kms_base_mixer_link_sink_pad (mixer, id, gp_name, VIDEO_SINK_PAD_NAME,
      internal_element, pad_name, MIXER_VIDEO_SRC_PAD,
      G_STRUCT_OFFSET (KmsBaseMixerPortData, video_sink_target));

  g_free (gp_name);
  return ret;
}

static gboolean
kms_base_mixer_link_audio_sink_default (KmsBaseMixer * mixer, gint id,
    GstElement * internal_element, const gchar * pad_name)
{
  gboolean ret;
  gchar *gp_name = g_strdup_printf (AUDIO_SINK_PAD_PREFIX "%d", id);

  ret = kms_base_mixer_link_sink_pad (mixer, id, gp_name, AUDIO_SINK_PAD_NAME,
      internal_element, pad_name, MIXER_AUDIO_SRC_PAD,
      G_STRUCT_OFFSET (KmsBaseMixerPortData, audio_sink_target));

  g_free (gp_name);
  return ret;
}

static void
kms_base_mixer_unhandle_port (KmsBaseMixer * mixer, gint id)
{
  KmsBaseMixerPortData *port_data;

  GST_DEBUG_OBJECT (mixer, "Unhandle port %" G_GINT32_FORMAT, id);

  port_data = (KmsBaseMixerPortData *) g_hash_table_lookup (mixer->priv->ports,
      &id);

  if (port_data == NULL)
    return;

  GST_DEBUG ("Removing element: %" GST_PTR_FORMAT, port_data->port);

  KMS_BASE_MIXER_LOCK (mixer);
  // TODO: Unlink end_point from mixer
  g_hash_table_remove (mixer->priv->ports, &id);
  KMS_BASE_MIXER_UNLOCK (mixer);
}

static gint *
kms_base_mixer_generate_port_id (KmsBaseMixer * mixer)
{
  gint *id;

  KMS_BASE_MIXER_LOCK (mixer);
  id = g_slice_new (gint);
  *id = mixer->priv->port_count++;
  KMS_BASE_MIXER_UNLOCK (mixer);

  return id;
}

static void
end_point_pad_added (GstElement * end_point, GstPad * pad,
    KmsBaseMixerPortData * port_data)
{
  if (gst_pad_get_direction (pad) == GST_PAD_SRC &&
      g_str_has_prefix (GST_OBJECT_NAME (pad), "mixer")) {

    KMS_BASE_MIXER_LOCK (port_data->mixer);

    if (port_data->video_sink_target != NULL
        && g_strstr_len (GST_OBJECT_NAME (pad), -1, "video")) {
      gchar *gp_name = g_strdup_printf (VIDEO_SINK_PAD_PREFIX "%d",
          port_data->id);

      GST_DEBUG_OBJECT (port_data->mixer,
          "Connect %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT, pad,
          port_data->video_sink_target);

      kms_base_mixer_create_and_link_ghost_pad (port_data->mixer, pad, gp_name,
          VIDEO_SINK_PAD_NAME, port_data->video_sink_target);
      g_free (gp_name);
    } else if (port_data->video_sink_target != NULL
        && g_strstr_len (GST_OBJECT_NAME (pad), -1, "audio")) {
      gchar *gp_name = g_strdup_printf (AUDIO_SINK_PAD_PREFIX "%d",
          port_data->id);

      GST_DEBUG_OBJECT (port_data->mixer,
          "Connect %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT, pad,
          port_data->audio_sink_target);

      kms_base_mixer_create_and_link_ghost_pad (port_data->mixer, pad, gp_name,
          AUDIO_SINK_PAD_NAME, port_data->audio_sink_target);
      g_free (gp_name);
    }

    KMS_BASE_MIXER_UNLOCK (port_data->mixer);
  }

}

static gint
kms_base_mixer_handle_port (KmsBaseMixer * mixer, GstElement * mixer_end_point)
{
  KmsBaseMixerPortData *port_data;
  gint *id;

  if (!KMS_IS_MIXER_END_POINT (mixer_end_point)) {
    GST_INFO_OBJECT (mixer, "Invalid MixerEndPoint: %" GST_PTR_FORMAT,
        mixer_end_point);
    return -1;
  }

  if (GST_OBJECT_PARENT (mixer) == NULL ||
      GST_OBJECT_PARENT (mixer) != GST_OBJECT_PARENT (mixer_end_point)) {
    GST_ERROR_OBJECT (mixer,
        "Mixer and MixerEndPoint do not have the same parent");
    return -1;
  }

  GST_DEBUG_OBJECT (mixer, "Handle port: %" GST_PTR_FORMAT, mixer_end_point);

  id = kms_base_mixer_generate_port_id (mixer);

  GST_DEBUG_OBJECT (mixer, "Adding new port %d", *id);
  port_data = kms_base_mixer_port_data_create (mixer, mixer_end_point, *id);

  g_signal_connect (G_OBJECT (mixer_end_point), "pad-added",
      G_CALLBACK (end_point_pad_added), port_data);

  KMS_BASE_MIXER_LOCK (mixer);
  g_hash_table_insert (mixer->priv->ports, id, port_data);
  KMS_BASE_MIXER_UNLOCK (mixer);

  return *id;
}

static void
kms_base_mixer_dispose (GObject * object)
{
  KmsBaseMixer *self = KMS_BASE_MIXER (object);

  if (self->priv->ports != NULL) {
    g_hash_table_unref (self->priv->ports);
    self->priv->ports = NULL;
  }

  G_OBJECT_CLASS (kms_base_mixer_parent_class)->dispose (object);
}

static void
kms_base_mixer_finalize (GObject * object)
{
  KmsBaseMixer *self = KMS_BASE_MIXER (object);

  g_rec_mutex_clear (&self->priv->mutex);

  G_OBJECT_CLASS (kms_base_mixer_parent_class)->finalize (object);
}

static void
kms_base_mixer_class_init (KmsBaseMixerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "BaseMixer", "Generic", "Kurento plugin for mixer connection",
      "Jose Antonio Santos Cadenas <santoscadenas@gmail.com>");

  klass->handle_port = GST_DEBUG_FUNCPTR (kms_base_mixer_handle_port);
  klass->unhandle_port = GST_DEBUG_FUNCPTR (kms_base_mixer_unhandle_port);

  klass->link_video_src =
      GST_DEBUG_FUNCPTR (kms_base_mixer_link_video_src_default);
  klass->link_audio_src =
      GST_DEBUG_FUNCPTR (kms_base_mixer_link_audio_src_default);
  klass->link_video_sink =
      GST_DEBUG_FUNCPTR (kms_base_mixer_link_video_sink_default);
  klass->link_audio_sink =
      GST_DEBUG_FUNCPTR (kms_base_mixer_link_audio_sink_default);

  gobject_class->dispose = GST_DEBUG_FUNCPTR (kms_base_mixer_dispose);
  gobject_class->finalize = GST_DEBUG_FUNCPTR (kms_base_mixer_finalize);

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&audio_src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&video_src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&audio_sink_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&video_sink_factory));

  /* Signals initialization */
  kms_base_mixer_signals[SIGNAL_HANDLE_PORT] =
      g_signal_new ("handle-port",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_ACTION | G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (KmsBaseMixerClass, handle_port), NULL, NULL,
      __kms_marshal_INT__OBJECT, G_TYPE_INT, 1, GST_TYPE_ELEMENT);

  kms_base_mixer_signals[SIGNAL_UNHANDLE_PORT] =
      g_signal_new ("unhandle-port",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_ACTION | G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (KmsBaseMixerClass, unhandle_port), NULL, NULL,
      __kms_marshal_VOID__INT, G_TYPE_NONE, 1, G_TYPE_INT);

  /* Registers a private structure for the instantiatable type */
  g_type_class_add_private (klass, sizeof (KmsBaseMixerPrivate));
}

static void
kms_base_mixer_init (KmsBaseMixer * self)
{
  self->priv = KMS_BASE_MIXER_GET_PRIVATE (self);

  g_rec_mutex_init (&self->priv->mutex);

  self->priv->port_count = 0;
  self->priv->ports = g_hash_table_new_full (g_int_hash, g_int_equal,
      release_gint, kms_base_mixer_port_data_destroy);
}