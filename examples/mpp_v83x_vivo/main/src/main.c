
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include "time.h"

#include "global_config.h"
#include "libmaix_debug.h"
#include "libmaix_err.h"
#include "libmaix_cam.h"
#include "libmaix_image.h"
#include "libmaix_disp.h"

#define CALC_FPS(tips)                                                                                     \
  {                                                                                                        \
    static int fcnt = 0;                                                                                   \
    fcnt++;                                                                                                \
    static struct timespec ts1, ts2;                                                                       \
    clock_gettime(CLOCK_MONOTONIC, &ts2);                                                                  \
    if ((ts2.tv_sec * 1000 + ts2.tv_nsec / 1000000) - (ts1.tv_sec * 1000 + ts1.tv_nsec / 1000000) >= 1000) \
    {                                                                                                      \
      printf("%s => H26X FPS:%d\n", tips, fcnt);                                                  \
      ts1 = ts2;                                                                                           \
      fcnt = 0;                                                                                            \
    }                                                                                                      \
  }

#include "sys/time.h"

static struct timeval old, now;

static void cap_set()
{
  gettimeofday(&old, NULL);
}

static void cap_get(const char *tips)
{
  gettimeofday(&now, NULL);
  if (now.tv_usec > old.tv_usec)
    printf("%20s - %5ld ms\r\n", tips, (now.tv_usec - old.tv_usec) / 1000);
}

struct {
  int w0, h0;
  struct libmaix_cam *cam0;
  #ifdef CONFIG_ARCH_V831 // CONFIG_ARCH_V831 & CONFIG_ARCH_V833
  struct libmaix_cam *cam1;
  #endif
  uint8_t *rgb888;

  uint8_t *yuv_buf;

  int w_vo, h_vo;
  struct libmaix_vo *vo;
  void *argb_vo;
  libmaix_image_t *argb_img;

  // struct libmaix_disp *disp;

  int is_run;
} test = { 0 };

static void test_handlesig(int signo)
{
  if (SIGINT == signo || SIGTSTP == signo || SIGTERM == signo || SIGQUIT == signo || SIGPIPE == signo || SIGKILL == signo)
  {
    test.is_run = 0;
  }
  // exit(0);
}

void test_init() {

  libmaix_camera_module_init();

  // test.w0 = 480, test.h0 = 270; // 16:9
  // test.w0 = 320, test.h0 = 240; // 4:3
  test.w0 = 240, test.h0 = 240; // 1:1

  test.cam0 = libmaix_cam_create(0, test.w0, test.h0, 1, 0);
  if (NULL == test.cam0) return ;  test.rgb888 = (uint8_t *)malloc(test.w0 * test.h0 * 3);

  test.yuv_buf = malloc(test.w0 * test.h0 * 3 / 2);
  if (NULL == test.yuv_buf)
    return;

  #ifdef CONFIG_ARCH_V831 // CONFIG_ARCH_V831 & CONFIG_ARCH_V833
  test.cam1 = libmaix_cam_create(1, test.w0, test.h0, 0, 0);
  if (NULL == test.cam0) return ;  test.rgb888 = (uint8_t *)malloc(test.w0 * test.h0 * 3);
  #endif

  // test.disp = libmaix_disp_create(0);
  // if(NULL == test.disp) return ;

  test.w_vo = 240, test.h_vo = 240;

  test.vo = libmaix_vo_create(test.w0, test.h0, 0, 0, test.w_vo, test.h_vo);
  if (NULL == test.vo) return ;

  // test.argb_vo = g2d_allocMem(test.w_vo * test.h_vo * 4);
  // if (NULL == test.argb_vo) return ;

  // test.argb_img = libmaix_image_create(test.w_vo, test.h_vo, LIBMAIX_IMAGE_MODE_RGBA8888, LIBMAIX_IMAGE_LAYOUT_HWC, test.argb_vo, false);

  test.argb_img = libmaix_image_create(test.w_vo, test.h_vo, LIBMAIX_IMAGE_MODE_RGBA8888, LIBMAIX_IMAGE_LAYOUT_HWC, NULL, false);

  test.is_run = 1;

  // ALOGE(__FUNCTION__);
}

void test_exit() {

  if (NULL != test.cam0) libmaix_cam_destroy(&test.cam0);

  if (NULL != test.yuv_buf)
    free(test.yuv_buf), test.yuv_buf = NULL;

  #ifdef CONFIG_ARCH_V831 // CONFIG_ARCH_V831 & CONFIG_ARCH_V833
  if (NULL != test.cam1) libmaix_cam_destroy(&test.cam1);
  #endif

  if (NULL != test.rgb888) free(test.rgb888), test.rgb888 = NULL;
  // if (NULL != test.disp) libmaix_disp_destroy(&test.disp), test.disp = NULL;

  if (NULL != test.vo)
    libmaix_vo_destroy(&test.vo), test.vo = NULL;

  // if (NULL != test.argb_vo)
  //   g2d_freeMem(test.argb_vo), test.argb_vo = NULL;

  if (test.argb_img)
      libmaix_image_destroy(&test.argb_img);

  libmaix_camera_module_deinit();

  // ALOGE(__FUNCTION__);
}

static libmaix_image_t *gray = NULL;

#include "apriltag.h"
#include "tag36h11.h"
#include "tag25h9.h"
#include "tag16h5.h"
#include "tagCircle21h7.h"
#include "tagCircle49h12.h"
#include "tagCustom48h12.h"
#include "tagStandard41h12.h"
#include "tagStandard52h13.h"

static const char *famname = "tag36h11";
static apriltag_detector_t *td = NULL;
static apriltag_family_t *tf = NULL;

static void apriltag_init()
{
    if (gray == NULL) {
        gray = libmaix_image_create(test.w0, test.h0, LIBMAIX_IMAGE_MODE_GRAY, LIBMAIX_IMAGE_LAYOUT_HWC, NULL, true);
        if (gray) {
            // Initialize tag detector with options
            if (!strcmp(famname, "tag36h11")) {
                tf = tag36h11_create();
            } else if (!strcmp(famname, "tag25h9")) {
                tf = tag25h9_create();
            } else if (!strcmp(famname, "tag16h5")) {
                tf = tag16h5_create();
            } else if (!strcmp(famname, "tagCircle21h7")) {
                tf = tagCircle21h7_create();
            } else if (!strcmp(famname, "tagCircle49h12")) {
                tf = tagCircle49h12_create();
            } else if (!strcmp(famname, "tagStandard41h12")) {
                tf = tagStandard41h12_create();
            } else if (!strcmp(famname, "tagStandard52h13")) {
                tf = tagStandard52h13_create();
            } else if (!strcmp(famname, "tagCustom48h12")) {
                tf = tagCustom48h12_create();
            } else {
                printf("Unrecognized tag family name. Use e.g. \"tag36h11\".\n");
                exit(-1);
            }

            td = apriltag_detector_create();
            apriltag_detector_add_family(td, tf);
            td->quad_decimate = 2.0; // "Decimate input image by this factor"
            td->quad_sigma = 0.0; // Apply low-pass blur to input
            td->nthreads = 1;
            td->debug = 0;
            td->refine_edges = 1;// Spend more time trying to align edges of tags

        }
    }
}

static void apriltag_exit()
{

    if (gray) {
        libmaix_image_destroy(&gray);

        apriltag_detector_destroy(td), td = NULL;

        if (!strcmp(famname, "tag36h11")) {
            tag36h11_destroy(tf);
        } else if (!strcmp(famname, "tag25h9")) {
            tag25h9_destroy(tf);
        } else if (!strcmp(famname, "tag16h5")) {
            tag16h5_destroy(tf);
        } else if (!strcmp(famname, "tagCircle21h7")) {
            tagCircle21h7_destroy(tf);
        } else if (!strcmp(famname, "tagCircle49h12")) {
            tagCircle49h12_destroy(tf);
        } else if (!strcmp(famname, "tagStandard41h12")) {
            tagStandard41h12_destroy(tf);
        } else if (!strcmp(famname, "tagStandard52h13")) {
            tagStandard52h13_destroy(tf);
        } else if (!strcmp(famname, "tagCustom48h12")) {
            tagCustom48h12_destroy(tf);
        }
        tf = NULL;
    }

}

static void apriltag_yuv_loop(uint8_t *data, uint32_t width, uint32_t height)
{
    if (gray) {

        libmaix_err_t err = LIBMAIX_ERR_NONE;

        // Make an image_u8_t header for the Mat data
        image_u8_t im = {
            .width = width,
            .height = height,
            .stride = width,
            .buf = data
        };

        zarray_t *detections = apriltag_detector_detect(td, &im);

        void *frame = test.vo->get_frame(test.vo, 9);

        if (frame) {
          uint32_t *vir = NULL, *phy = NULL;
          test.vo->frame_addr(test.vo, frame, &vir, &phy);

          test.argb_img->data = vir[0], memset(test.argb_img->data, 0, test.argb_img->width * test.argb_img->height * 4);

          // Draw detection outlines
          for (int i = 0; i < zarray_size(detections); i++) {

            apriltag_detection_t *det;
            zarray_get(detections, i, &det);

              libmaix_cv_image_draw_line(test.argb_img, (int)det->p[0][0], (int)det->p[0][1], (int)det->p[1][0], (int)det->p[1][1], MaixColorBGRA(0xff, 0, 0, 0xff), 2);
              libmaix_cv_image_draw_line(test.argb_img, (int)det->p[0][0], (int)det->p[0][1], (int)det->p[3][0], (int)det->p[3][1], MaixColorBGRA(0xff, 0, 0, 0xff), 2);
              libmaix_cv_image_draw_line(test.argb_img, (int)det->p[1][0], (int)det->p[1][1], (int)det->p[2][0], (int)det->p[2][1], MaixColorBGRA(0xff, 0, 0, 0xff), 2);
              libmaix_cv_image_draw_line(test.argb_img, (int)det->p[2][0], (int)det->p[2][1], (int)det->p[3][0], (int)det->p[3][1], MaixColorBGRA(0xff, 0, 0, 0xff), 2);

              char det_id[8] = {0};
              sprintf(det_id, "%d", det->id);
              int w, h;
              libmaix_cv_image_get_string_size(&w, &h, det_id, 1.5, 1);
              libmaix_cv_image_draw_string(test.argb_img, (int)(det->c[0]), (int)(det->c[1]) - (h / 2), det_id, 1.5, MaixColorBGRA(0xff, 255, 0, 0), 1);

              // printf("\r\n[(%s,%.02f):(%d,%d)]:[(%d:%d),(%d:%d)]:[(%d:%d),(%d:%d)]:[(%d:%d),(%d:%d)]:[(%d:%d),(%d:%d)]\r\n",
              //     det_id, det->decision_margin, (int)det->c[0], (int)det->c[1],
              //     (int)det->p[0][0], (int)det->p[0][1], (int)det->p[1][0], (int)det->p[1][1],
              //     (int)det->p[1][0], (int)det->p[1][1], (int)det->p[3][0], (int)det->p[3][1],
              //     (int)det->p[1][0], (int)det->p[1][1], (int)det->p[2][0], (int)det->p[2][1],
              //     (int)det->p[2][0], (int)det->p[2][1], (int)det->p[3][0], (int)det->p[3][1]
              // );
          }

          test.vo->set_frame(test.vo, frame, 9);
        }


        apriltag_detections_destroy(detections);

    }
}

static void apriltag_loop(libmaix_image_t* img)
{
    if (gray) {

        libmaix_err_t err = LIBMAIX_ERR_NONE;

        libmaix_cv_image_convert(img, LIBMAIX_IMAGE_MODE_GRAY, &gray);

        // Make an image_u8_t header for the Mat data
        image_u8_t im = {
            .width = gray->width,
            .height = gray->height,
            .stride = gray->width,
            .buf = gray->data
        };

        zarray_t *detections = apriltag_detector_detect(td, &im);

        // Draw detection outlines
        for (int i = 0; i < zarray_size(detections); i++) {
          apriltag_detection_t *det;
          zarray_get(detections, i, &det);

          libmaix_cv_image_draw_line(img, (int)det->p[0][0], (int)det->p[0][1], (int)det->p[1][0], (int)det->p[1][1], MaixColor(0, 0xff, 0), 2);
          libmaix_cv_image_draw_line(img, (int)det->p[0][0], (int)det->p[0][1], (int)det->p[3][0], (int)det->p[3][1], MaixColor(0, 0xff, 0), 2);
          libmaix_cv_image_draw_line(img, (int)det->p[1][0], (int)det->p[1][1], (int)det->p[2][0], (int)det->p[2][1], MaixColor(0, 0xff, 0), 2);
          libmaix_cv_image_draw_line(img, (int)det->p[2][0], (int)det->p[2][1], (int)det->p[3][0], (int)det->p[3][1], MaixColor(0, 0xff, 0), 2);

          char det_id[8] = {0};
          sprintf(det_id, "%d", det->id);
          int w, h;
          libmaix_cv_image_get_string_size(&w, &h, det_id, 1.5, 1);
          libmaix_cv_image_draw_string(img, (int)(det->c[0]), (int)(det->c[1]) - (h / 2), det_id, 1.5, MaixColor(255, 0, 0), 1);

          // printf("\r\n[(%s,%.02f):(%d,%d)]:[(%d:%d),(%d:%d)]:[(%d:%d),(%d:%d)]:[(%d:%d),(%d:%d)]:[(%d:%d),(%d:%d)]\r\n",
          //     det_id, det->decision_margin, (int)det->c[0], (int)det->c[1],
          //     (int)det->p[0][0], (int)det->p[0][1], (int)det->p[1][0], (int)det->p[1][1],
          //     (int)det->p[1][0], (int)det->p[1][1], (int)det->p[3][0], (int)det->p[3][1],
          //     (int)det->p[1][0], (int)det->p[1][1], (int)det->p[2][0], (int)det->p[2][1],
          //     (int)det->p[2][0], (int)det->p[2][1], (int)det->p[3][0], (int)det->p[3][1]
          // );

        }

        apriltag_detections_destroy(detections);

    }
}

#include "zbar.h"

static zbar_image_scanner_t *scanner = NULL;

static void qrcode_init()
{
    if (gray == NULL) {
        gray = libmaix_image_create(test.w0, test.h0, LIBMAIX_IMAGE_MODE_GRAY, LIBMAIX_IMAGE_LAYOUT_HWC, NULL, true);
        if (gray) {
            /* create a reader */
            scanner = zbar_image_scanner_create();
            /* configure the reader */
            // zbar_image_scanner_set_config(scanner, 0, ZBAR_CFG_ENABLE, 1);
            // zbar_image_scanner_set_config(scanner, 0, ZBAR_CFG_POSITION, 1);
            // zbar_image_scanner_set_config(scanner, 0, ZBAR_CFG_UNCERTAINTY, 2);
            // zbar_image_scanner_set_config(scanner, ZBAR_QRCODE, ZBAR_CFG_UNCERTAINTY, 0);
            // zbar_image_scanner_set_config(scanner, ZBAR_CODE128, ZBAR_CFG_UNCERTAINTY, 0);
            // zbar_image_scanner_set_config(scanner, ZBAR_CODE93, ZBAR_CFG_UNCERTAINTY, 0);
            // zbar_image_scanner_set_config(scanner, ZBAR_CODE39, ZBAR_CFG_UNCERTAINTY, 0);
            // zbar_image_scanner_set_config(scanner, ZBAR_CODABAR, ZBAR_CFG_UNCERTAINTY, 0);
            // zbar_image_scanner_set_config(scanner, ZBAR_COMPOSITE, ZBAR_CFG_UNCERTAINTY, 0);
            // zbar_image_scanner_set_config(scanner, ZBAR_PDF417, ZBAR_CFG_UNCERTAINTY, 0);
            // zbar_image_scanner_set_config(scanner, ZBAR_CODABAR, ZBAR_CFG_UNCERTAINTY, 0);
            // zbar_image_scanner_set_config(scanner, ZBAR_COMPOSITE, ZBAR_CFG_UNCERTAINTY, 0);
            // zbar_image_scanner_set_config(scanner, ZBAR_PDF417, ZBAR_CFG_UNCERTAINTY, 0);
            // zbar_image_scanner_set_config(scanner, ZBAR_DATABAR, ZBAR_CFG_UNCERTAINTY, 0);
        }
    }

}

static void qrcode_exit()
{
    if (gray) {
        libmaix_image_destroy(&gray);
        if (scanner != NULL) {
          zbar_image_scanner_destroy(scanner), scanner = NULL;
        }
    }
}

#define zbar_fourcc(a, b, c, d)                 \
        ((unsigned long)(a) |                   \
         ((unsigned long)(b) << 8) |            \
         ((unsigned long)(c) << 16) |           \
         ((unsigned long)(d) << 24))

static void qrcode_loop(libmaix_image_t* img)
{
    if (gray) {
        libmaix_err_t err = LIBMAIX_ERR_NONE;

        libmaix_cv_image_convert(img, LIBMAIX_IMAGE_MODE_GRAY, &gray);

        /* obtain image data */
        int width = gray->width, height = gray->height;
        uint8_t *raw = gray->data;

        /* wrap image data */
        zbar_image_t *image = zbar_image_create();
        zbar_image_set_format(image, zbar_fourcc('Y','8','0','0'));
        zbar_image_set_size(image, width, height);
        zbar_image_set_data(image, raw, width * height, NULL);

        // cap_set();

        /* scan the image for barcodes */
        int n = zbar_scan_image(scanner, image);

        // cap_get("zbar_scan_image");

        /* extract results */
        const zbar_symbol_t *symbol = zbar_image_first_symbol(image);
        for(; symbol; symbol = zbar_symbol_next(symbol)) {
            /* do something useful with results */
            zbar_symbol_type_t typ = zbar_symbol_get_type(symbol);
            const char *data = zbar_symbol_get_data(symbol);
            if (typ == ZBAR_QRCODE) {
                int datalen = strlen(data);
                // libmaix_cv_image_draw_string(img, 0, 0, zbar_get_symbol_name(typ), 1.5, MaixColor(0, 0, 255), 1);
                // libmaix_cv_image_draw_string(img, 0, 20, data, 1.5, MaixColor(255, 0, 0), 1);
                printf("decoded %s symbol \"%s\"\n", zbar_get_symbol_name(typ), data);
                // break;
            }
        }

        /* clean up */
        zbar_image_destroy(image); // use zbar_image_free_data
    }

}

static void qrcode_yuv_loop(uint8_t *raw, uint32_t width, uint32_t height)
{
    if (gray) {
        libmaix_err_t err = LIBMAIX_ERR_NONE;

        /* wrap image data */
        zbar_image_t *image = zbar_image_create();
        zbar_image_set_format(image, zbar_fourcc('Y','8','0','0'));
        zbar_image_set_size(image, width, height);
        zbar_image_set_data(image, raw, width * height, NULL);

        // cap_set();

        /* scan the image for barcodes */
        int n = zbar_scan_image(scanner, image);

        // cap_get("zbar_scan_image");

        /* extract results */
        const zbar_symbol_t *symbol = zbar_image_first_symbol(image);
        for(; symbol; symbol = zbar_symbol_next(symbol)) {
            /* do something useful with results */
            zbar_symbol_type_t typ = zbar_symbol_get_type(symbol);
            const char *data = zbar_symbol_get_data(symbol);
            if (typ == ZBAR_QRCODE) {
                int datalen = strlen(data);
                // libmaix_cv_image_draw_string(img, 0, 0, zbar_get_symbol_name(typ), 1.5, MaixColor(0, 0, 255), 1);
                // libmaix_cv_image_draw_string(img, 0, 20, data, 1.5, MaixColor(255, 0, 0), 1);
                printf("decoded %s symbol \"%s\"\n", zbar_get_symbol_name(typ), data);

                void *frame = test.vo->get_frame(test.vo, 9);
                // printf("get_frame %p\r\n", frame);
                if (frame) {
                  uint32_t *vir = NULL, *phy = NULL;
                  test.vo->frame_addr(test.vo, frame, &vir, &phy);

                  test.argb_img->data = vir[0], memset(test.argb_img->data, 0, test.argb_img->width * test.argb_img->height * 4);

                  libmaix_cv_image_draw_string(test.argb_img, 0, 0, zbar_get_symbol_name(typ), 1.5, MaixColorBGRA(0xff, 0, 0, 0xff), 1);
                  libmaix_cv_image_draw_string(test.argb_img, 0, 20, data, 1.5, MaixColorBGRA(0xff, 0, 0, 0xff), 1);

                  test.vo->set_frame(test.vo, frame, 9);
                }

                // break;
            }
        }

        /* clean up */
        zbar_image_destroy(image); // use zbar_image_free_data
    }

}

// void test_voui() {
//     void *frame = test.vo->get_frame(test.vo, 9);
//     // printf("get_frame %p\r\n", frame);
//     if (frame) {
//       uint32_t *vir = NULL, *phy = NULL;
//       test.vo->frame_addr(test.vo, frame, &vir, &phy);
//       void * p_vir_addr = test.argb_vo;
//       void * p_phy_addr = (g2d_getPhyAddrByVirAddr(p_vir_addr));
//       if (p_vir_addr) {
//         // printf("p_vir_addr %p\r\n", p_vir_addr);
//         // _g2d_argb_rotate((void *)p_phy_addr, (void *)phy[0], test.h_vo, test.w_vo, 1);
//         test.vo->set_frame(test.vo, frame, 9);
//       }
//     }
// }

void test_work() {

  test.cam0->start_capture(test.cam0);

  #ifdef CONFIG_ARCH_V831 // CONFIG_ARCH_V831 & CONFIG_ARCH_V833
  test.cam1->start_capture(test.cam1);
  #endif

  while (test.is_run)
  {
    if (LIBMAIX_ERR_NONE == test.cam0->capture(test.cam0, test.yuv_buf)) {
      // puts("maix_test 1");
      void *frame = test.vo->get_frame(test.vo, 0);
      if (frame != NULL) {
        unsigned int *addr = NULL;
        test.vo->frame_addr(test.vo, frame, &addr, NULL);
        // g2d_nv21_rotate(test.yuv_buf0, test.w0, test.h0, 3);
        memcpy((void *)addr[0], test.yuv_buf, test.w0 * test.h0 * 3 / 2);
        test.vo->set_frame(test.vo, frame, 0);
      }

      // qrcode_yuv_loop(test.yuv_buf, test.w0, test.h0);

      // apriltag_yuv_loop(test.yuv_buf, test.w0, test.h0);

    }

    if (LIBMAIX_ERR_NONE == test.cam1->capture(test.cam1, test.yuv_buf)) {

    }

    CALC_FPS("test_vivo");
  }
}

#include "lvgl/lvgl.h"
#include "lv_examples/lv_examples.h"
#include "lv_lib_png/lv_png.h"
#include "evdev_mouse.h"
#define EVDEV_MOUSE_NAME    "/dev/input/event1"

static lv_color_t *s_buf1 = NULL;
static lv_color_t *s_buf2 = NULL;

void _lvgl_ui__flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
  if (test.vo)
  {
    void *frame = test.vo->get_frame(test.vo, 9);
    if (frame) {
      uint32_t *vir = NULL, *phy = NULL;
      test.vo->frame_addr(test.vo, frame, &vir, &phy);
      // test.argb_img->data = vir[0], memset(test.argb_img->data, 0, test.argb_img->width * test.argb_img->height * 4);
      memcpy((uint32_t *)vir[0], color_p, lv_area_get_size(area) * sizeof(lv_color_t));
      test.vo->set_frame(test.vo, frame, 9);
      // uint32_t tmp = ((uint32_t *)vir[0])[0];
      // printf("_lvgl_ui__flush area %X\r\n", tmp);

    }
    lv_disp_flush_ready(drv);
  }
}

void gui_init(uint32_t disp_width, uint32_t disp_height)
{
  lv_init();

  uint32_t size_in_px_cnt = disp_width * disp_height;
  s_buf1 = (lv_color_t *)malloc(size_in_px_cnt * sizeof(lv_color_t));
  assert(s_buf1);
  s_buf2 = (lv_color_t *)malloc(size_in_px_cnt * sizeof(lv_color_t));
  assert(s_buf2);

  /*Create a display buffer*/
  static lv_disp_buf_t disp_buf;
  lv_disp_buf_init(&disp_buf, s_buf1, s_buf2, size_in_px_cnt);

  /*Create a display*/
  lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.buffer = &disp_buf;
  disp_drv.hor_res = disp_width;
  disp_drv.ver_res = disp_height;
  disp_drv.flush_cb = _lvgl_ui__flush;
  lv_disp_drv_register(&disp_drv);

  evdev_mouse_set_file(EVDEV_MOUSE_NAME);
  lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = evdev_mouse_read;
  lv_indev_t *mouse_indev = lv_indev_drv_register(&indev_drv);

  /*Set a cursor for the mouse*/
#if 1
  LV_IMG_DECLARE(touch);                                    /*Declare the image file.*/
  lv_obj_t *cursor_obj = lv_img_create(lv_scr_act(), NULL); /*Create an image object for the cursor */
  lv_img_set_src(cursor_obj, &touch);                       /*Set the image source*/
#else
  lv_obj_t *cursor_obj = lv_label_create(lv_scr_act(), NULL);
  lv_label_set_recolor(cursor_obj, true);
  lv_label_set_text(cursor_obj, "#ff0000 .cursor");
#endif

  lv_indev_set_cursor(mouse_indev, cursor_obj);

  /* Optional:
      * Create a memory monitor task which prints the memory usage in
      * periodically.*/
  // lv_task_create(memory_monitor, 5000, LV_TASK_PRIO_MID, NULL);

  lv_port_fs_init();

  lv_png_init();

  static lv_style_t screen_style;
  lv_style_init(&screen_style);
  // lv_style_set_bg_opa(&screen_style, LV_STATE_DEFAULT, LV_OPA_0);
  lv_style_set_bg_color(&screen_style, LV_STATE_DEFAULT, (lv_color_t){0x00, 0x00, 0x00, 0x00});
  lv_obj_add_style(lv_scr_act(), LV_OBJ_PART_MAIN, &screen_style); /*Default button style*/
  lv_obj_add_style(lv_scr_act(), LV_BTN_PART_MAIN, &screen_style); /*Default button style*/
  lv_obj_add_style(lv_scr_act(), LV_IMG_PART_MAIN, &screen_style); /*Default button style*/

  // lv_obj_t *screen = lv_obj_create(lv_scr_act(), NULL);
  // lv_obj_set_size(screen, disp_width, disp_height);
  // lv_obj_add_style(screen, LV_OBJ_PART_MAIN, &screen_style);
  // lv_obj_align(screen, NULL, LV_ALIGN_CENTER, 0, 0);

  // lv_obj_t *obj = lv_img_create(lv_scr_act(), NULL);
  // lv_img_set_src(obj, "/home/res/logo.png");
  // lv_obj_add_style(obj, LV_STATE_DEFAULT, &screen_style);
  // lv_obj_align(obj, NULL, LV_ALIGN_CENTER, 0, 0);

  // example test
  lv_ex_get_started_1();
  lv_ex_get_started_2();
  // lv_ex_get_started_3();

  lv_ex_style_7();
  lv_ex_style_8();
  lv_ex_style_9();

  // lv_demo_widgets();

}

void gui_exit(void)
{
  if (s_buf1)
    free(s_buf1), s_buf1 = NULL;
  if (s_buf1)
    free(s_buf2), s_buf2 = NULL;
}

// ==================

#include <pthread.h>
pthread_t th;
int th_usec = 0;

void *thread(void *arg)
{
  while (th_usec)
  {
    lv_task_handler();
    usleep(th_usec);
  }
  return 0;
}

int _gui_loop_set_timer(int tv_usec)
{
  th_usec = tv_usec;
  int ret = pthread_create(&th, NULL, thread, &th_usec);
  return (ret != 0) ? -1 : 0;
}

void _gui_loop_clr_timer()
{
  int *thread_ret = NULL;
  if (th_usec)
  {
    th_usec = 0;
    pthread_join(th, (void **)&thread_ret);
  }
}

// ==================

// void __gui_loop__(int signo)
// {
//   lv_task_handler();
// }

// void gui_loop_set_timer(int tv_usec)
// {
//   struct sigaction act;
//   act.sa_handler = __gui_loop__;
//   act.sa_flags = 0;
//   sigemptyset(&act.sa_mask);
//   sigaction(SIGPROF, &act, NULL); //设置信号 SIGPROF 的处理函数为 print_info

//   struct itimerval value;
//   value.it_value.tv_sec = 0;
//   value.it_value.tv_usec = tv_usec;
//   value.it_interval = value.it_value;
//   setitimer(ITIMER_PROF, &value, NULL); //初始化 timer，到期发送 SIGPROF 信号
// }

// void gui_loop_clr_timer()
// {
//   struct itimerval value;
//   value.it_value.tv_sec = 0;
//   value.it_value.tv_usec = 0;
//   value.it_interval = value.it_value;
//   setitimer(ITIMER_PROF, &value, NULL);
// }

// ==================

int main(int argc, char **argv)
{
  signal(SIGINT, test_handlesig);
  signal(SIGTERM, test_handlesig);

  libmaix_image_module_init();

  test_init();

  gui_init(test.w_vo, test.h_vo);

  _gui_loop_set_timer(50*1000);

  test_work();

  _gui_loop_clr_timer();

  gui_exit();

  test_exit();

  libmaix_image_module_deinit();

  return 0;
}