
#include "libmaix_cv_image.h"

#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs/legacy/constants_c.h>
#include "opencv2/core/types_c.h"
#include <opencv2/core/core.hpp>
#include <opencv2/freetype.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/freetype.hpp>

void overlayImage(const cv::Mat &background, const cv::Mat &foreground,
                  cv::Mat &output, cv::Point2i location)
{
    background.copyTo(output);
    // start at the row indicated by location, or at row 0 if location.y is negative.
    for (int y = std::max(location.y, 0); y < background.rows; ++y)
    {
        int fY = y - location.y; // because of the translation
        // we are done of we have processed all rows of the foreground image.
        if (fY >= foreground.rows)
            break;

        // start at the column indicated by location,
        // or at column 0 if location.x is negative.
        for (int x = std::max(location.x, 0); x < background.cols; ++x)
        {
            int fX = x - location.x; // because of the translation.
            // we are done with this row if the column is outside of the foreground image.
            if (fX >= foreground.cols)
                break;

            // determine the opacity of the foregrond pixel, using its fourth (alpha) channel.
            double opacity =
                ((double)foreground.data[fY * foreground.step + fX * foreground.channels() + 3]) / 255.;

            // and now combine the background and foreground pixel, using the opacity,
            // but only if opacity > 0.
            for (int c = 0; opacity > 0 && c < output.channels(); ++c)
            {
                unsigned char foregroundPx =
                    foreground.data[fY * foreground.step + fX * foreground.channels() + c];
                unsigned char backgroundPx =
                    background.data[y * background.step + x * background.channels() + c];
                output.data[y * output.step + output.channels() * x + c] =
                    backgroundPx * (1. - opacity) + foregroundPx * opacity;
            }
        }
    }
}

//图片混合
bool mergeImage(cv::Mat &srcImage, cv::Mat mixImage, cv::Point startPoint)
{
    //检查图片数据
    if (!srcImage.data || !mixImage.data)
    {
        // cout << "输入图片 数据错误！" << endl ;
        return LIBMAIX_ERR_PARAM;
    }
    //检查行列是否越界
    int addCols = startPoint.x + mixImage.cols > srcImage.cols ? 0 : mixImage.cols;
    int addRows = startPoint.y + mixImage.rows > srcImage.rows ? 0 : mixImage.rows;
    if (addCols == 0 || addRows == 0)
    {
        // cout << "添加图片超出" << endl;
        return LIBMAIX_ERR_PARAM;
    }

    //ROI 混合区域
    cv::Mat roiImage = srcImage(cv::Rect(startPoint.x, startPoint.y, addCols, addRows));

    // printf("MixImage %d %d\r\n", srcImage.type() == CV_8UC3, mixImage.type() == CV_8UC4);

    //图片类型一致
    if (srcImage.type() == mixImage.type())
    {
        puts("copyTo");
        mixImage.copyTo(roiImage, mixImage);
        return LIBMAIX_ERR_NONE;
    }

    cv::Mat maskImage;

    //原始图片：RGB  贴图：ARGB
    if (srcImage.type() == CV_8UC3 && mixImage.type() == CV_8UC4)
    {
        cv::cvtColor(mixImage, maskImage, cv::ColorConversionCodes::COLOR_RGBA2BGR);
        maskImage.copyTo(roiImage, maskImage);
        return LIBMAIX_ERR_NONE;
    }

    //原始图片：灰度  贴图：彩色
    if (srcImage.type() == CV_8U && mixImage.type() == CV_8UC3)
    {
        cv::cvtColor(mixImage, maskImage, cv::ColorConversionCodes::COLOR_BGR2GRAY);
        maskImage.copyTo(roiImage, maskImage);
        return LIBMAIX_ERR_NONE;
    }

    //原始图片：彩色  贴图：灰色
    if (srcImage.type() == CV_8UC3 && mixImage.type() == CV_8U)
    {
        cv::cvtColor(mixImage, maskImage, cv::ColorConversionCodes::COLOR_GRAY2BGR);
        maskImage.copyTo(roiImage, maskImage);
        return LIBMAIX_ERR_NONE;
    }

    return LIBMAIX_ERR_NOT_IMPLEMENT;
}

class libmaix_font {
public:
    static cv::Ptr<cv::freetype::FreeType2> ft;
    static bool is_load;
};

cv::Ptr<cv::freetype::FreeType2> libmaix_font::ft = cv::freetype::createFreeType2();
bool libmaix_font::is_load = false;

extern "C"
{
    #include "libmaix_debug.h"

    libmaix_err_t libmaix_cv_image_draw_ellipse(libmaix_image_t *src, int x, int y, int w, int h, double angle, double startAngle, double endAngle, libmaix_image_color_t color, int thickness)
    {
        if (src->data == NULL)
        {
            return LIBMAIX_ERR_PARAM;
        }
        if (src->mode == LIBMAIX_IMAGE_MODE_RGB888)
        {
            cv::Mat input(src->width, src->height, CV_8UC3, const_cast<char *>((char *)src->data));
            cv::ellipse(input, cv::Point(x, y), cv::Size(w, h), angle, startAngle, endAngle, cv::Scalar(color.rgb888.r, color.rgb888.g, color.rgb888.b), thickness);
            // memcpy(src->data, input.data, src->width * src->height * 3);
            return LIBMAIX_ERR_NONE;
        }
        return LIBMAIX_ERR_NOT_IMPLEMENT;
    }

    libmaix_err_t libmaix_cv_image_draw_circle(libmaix_image_t *src, int x, int y, int r, libmaix_image_color_t color, int thickness)
    {
        if (src->data == NULL)
        {
            return LIBMAIX_ERR_PARAM;
        }
        if (src->mode == LIBMAIX_IMAGE_MODE_RGB888)
        {
            cv::Mat input(src->width, src->height, CV_8UC3, src->data);
            cv::circle(input, cv::Point(x, y), r, cv::Scalar(color.rgb888.r, color.rgb888.g, color.rgb888.b), thickness);
            // memcpy(src->data, input.data, src->width * src->height * 3);
            return LIBMAIX_ERR_NONE;
        }
        return LIBMAIX_ERR_NOT_IMPLEMENT;
    }

    libmaix_err_t libmaix_cv_image_draw_rectangle(libmaix_image_t *src, int x1, int y1, int x2, int y2, libmaix_image_color_t color, int thickness)
    {
        if (src->data == NULL)
        {
            return LIBMAIX_ERR_PARAM;
        }
        if (src->mode == LIBMAIX_IMAGE_MODE_RGB888)
        {
            cv::Mat input(src->width, src->height, CV_8UC3, src->data);
            cv::rectangle(input, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(color.rgb888.r, color.rgb888.g, color.rgb888.b), thickness);
            // memcpy(src->data, input.data, src->width * src->height * 3);
            return LIBMAIX_ERR_NONE;
        }
        return LIBMAIX_ERR_NOT_IMPLEMENT;
    }

    libmaix_err_t libmaix_cv_image_draw_line(libmaix_image_t *src, int x1, int y1, int x2, int y2, libmaix_image_color_t color, int thickness)
    {
        if (src->data == NULL)
        {
            return LIBMAIX_ERR_PARAM;
        }
        if (src->mode == LIBMAIX_IMAGE_MODE_RGB888)
        {
            cv::Mat input(src->width, src->height, CV_8UC3, src->data);
            cv::line(input, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(color.rgb888.r, color.rgb888.g, color.rgb888.b), thickness);
            // memcpy(src->data, input.data, src->width * src->height * 3);
            return LIBMAIX_ERR_NONE;
        }
        return LIBMAIX_ERR_NOT_IMPLEMENT;
    }

    libmaix_err_t libmaix_cv_image_draw_image(libmaix_image_t *src, int x, int y, libmaix_image_t *dst)
    {
        if (src->data == NULL || dst->data == NULL)
        {
            return LIBMAIX_ERR_PARAM;
        }
        if (src->mode == LIBMAIX_IMAGE_MODE_RGB888 && src->mode == dst->mode)
        {
            cv::Mat input(src->width, src->height, CV_8UC3, src->data);
            cv::Mat temp(dst->width, dst->height, CV_8UC3, dst->data);
            mergeImage(input, temp, cv::Point(x, y));
            // memcpy(src->data, input.data, src->width * src->height * 3);
            return LIBMAIX_ERR_NONE;
        }
        return LIBMAIX_ERR_NOT_IMPLEMENT;
    }

    libmaix_err_t libmaix_cv_image_draw_image_open(libmaix_image_t *src, int x, int y, const char *path)
    {
        if (src->data == NULL)
        {
            return LIBMAIX_ERR_PARAM;
        }
        if (src->mode == LIBMAIX_IMAGE_MODE_RGB888)
        {
            cv::Mat input(src->width, src->height, CV_8UC3, src->data);
            cv::Mat image = cv::imread(path, CV_LOAD_IMAGE_UNCHANGED); // maybe need export
            if (!image.empty())
            {
                mergeImage(input, image, cv::Point(x, y));
                // memcpy(src->data, input.data, src->width * src->height * 3);
                return LIBMAIX_ERR_NONE;
            }
            return LIBMAIX_ERR_NOT_READY;
        }
        return LIBMAIX_ERR_NOT_IMPLEMENT;
    }

    libmaix_err_t libmaix_cv_image_load_freetype(const char *path)
    {
        if (libmaix_font::is_load) libmaix_font::ft = cv::freetype::createFreeType2(); // re-load clear it
        libmaix_font::ft->loadFontData(path, 0);
        libmaix_font::is_load = true;
        return LIBMAIX_ERR_NONE;
    }

    // libmaix_err_t libmaix_cv_image_font_free()
    // {
    //     delete libmaix_font::ft;
    //     return LIBMAIX_ERR_NONE;
    // }

    libmaix_err_t libmaix_cv_image_draw_string(libmaix_image_t *src, int x, int y, const char *str, double scale, libmaix_image_color_t color, int thickness)
    {
        if (src->data == NULL)
        {
            return LIBMAIX_ERR_PARAM;
        }
        if (src->mode == LIBMAIX_IMAGE_MODE_RGB888)
        {
            int fontHeight = 32 * scale; // default 32
            cv::Mat input(src->width, src->height, CV_8UC3, src->data);
            cv::String text(str);
            if (!libmaix_font::is_load) {
                cv::putText(input, text, cv::Point(x, y + fontHeight), 0, scale, cv::Scalar(color.rgb888.r, color.rgb888.g, color.rgb888.b), thickness);
            } else {
                libmaix_font::ft->putText(input, text, cv::Point(x, y + fontHeight), fontHeight, cv::Scalar(color.rgb888.r, color.rgb888.g, color.rgb888.b), -1, 8, true);
            }
            return LIBMAIX_ERR_NOT_READY;
        }
        return LIBMAIX_ERR_NOT_IMPLEMENT;
    }

    static inline int libmaix_cv_image_load(struct libmaix_image *src, struct libmaix_image **dst)
    {
        int new_mem = 0;
        if((*dst) == NULL)
        {
            *dst = libmaix_image_create(src->width, src->height, src->mode, src->layout, NULL, true);
            if(!(*dst))
            {
            return LIBMAIX_ERR_NO_MEM;
            }
            new_mem = 1;
        }
        else
        {
            if( (*dst)->data == NULL)
            {
            (*dst)->data = malloc(src->width * src->height * 3);
            if(!(*dst)->data)
            {
                return LIBMAIX_ERR_NO_MEM;
            }
            (*dst)->is_data_alloc = true;
            new_mem = 2;
            }
            (*dst)->layout = src->layout;
            (*dst)->width = src->width;
            (*dst)->height = src->height;
        }
        return new_mem;
    }

    static inline void libmaix_cv_image_free(int err, int new_mem, struct libmaix_image **dst)
    {
        if(err != LIBMAIX_ERR_NONE)
        {
            if(new_mem == 2)
            {
            free((*dst)->data);
            (*dst)->data = NULL;
            }
            else if (new_mem == 1)
            {
            libmaix_image_destroy(dst);
            }
        }
    }

    libmaix_err_t libmaix_cv_image_convert(struct libmaix_image *src, libmaix_image_mode_t mode, struct libmaix_image **dst)
    {
        libmaix_err_t err = LIBMAIX_ERR_NONE;
        if(dst == NULL)
        {
            return LIBMAIX_ERR_PARAM;
        }
        if(mode == src->mode)
        {
            return LIBMAIX_ERR_NONE;
        }
        if(src->width==0 || src->height==0 || src->data == NULL)
        {
            return LIBMAIX_ERR_PARAM;
        }

        int new_mem = libmaix_cv_image_load(src, dst);
        // -------------------------------
        switch(src->mode)
        {
            case LIBMAIX_IMAGE_MODE_RGB888: {
            switch (mode)
            {
                case LIBMAIX_IMAGE_MODE_RGB565: {
                if (src == *dst || src->width != (*dst)->width || src->height != (*dst)->height) return LIBMAIX_ERR_PARAM;
                uint8_t *rgb888 = (uint8_t *)src->data;
                uint16_t *rgb565 = (uint16_t *)(*dst)->data;
                for (uint16_t *end = rgb565 + src->width * src->height; rgb565 < end; rgb565 += 1, rgb888 += 3) {
                    // *rgb565 = make16color(rgb888[0], rgb888[1], rgb888[2]);
                    *rgb565 = ((((rgb888[0] >> 3) & 31) << 11) | (((rgb888[1] >> 2) & 63) << 5) | ((rgb888[2] >> 3) & 31));
                }
                (*dst)->mode = mode;
                break;
                }
                case LIBMAIX_IMAGE_MODE_BGR888: {
                // printf("libmaix_image_hal_convert src->mode %d mode %d \r\n", src->mode, mode);
                uint8_t *rgb = (uint8_t *)(src->data), *bgr = (uint8_t *)(*dst)->data;
                for (uint8_t *end = rgb + src->width * src->height * 3; rgb < end; rgb += 3, bgr += 3) {
                    bgr[2] = rgb[0], bgr[1] = rgb[1], bgr[0] = rgb[2];
                }
                (*dst)->mode = mode;
                break;
                }
                default:
                err = LIBMAIX_ERR_PARAM;
                break;
            }
            break;
            }
            default:
            err = LIBMAIX_ERR_NOT_IMPLEMENT;
            break;
        }
        // -------------------------------
        libmaix_cv_image_free(err, new_mem, dst);

        return err;
    }

    libmaix_err_t libmaix_cv_image_resize(struct libmaix_image *src, int w, int h, struct libmaix_image **dst)
    {
        libmaix_err_t err = LIBMAIX_ERR_NONE;
        if(dst == NULL)
        {
            return LIBMAIX_ERR_PARAM;
        }
        if(src->width==0 || src->height==0 || src->data == NULL)
        {
            return LIBMAIX_ERR_PARAM;
        }

        int new_mem = libmaix_cv_image_load(src, dst);
        // -------------------------------
        LIBMAIX_IMAGE_ERROR(LIBMAIX_ERR_NOT_IMPLEMENT);
        // -------------------------------
        libmaix_cv_image_free(err, new_mem, dst);
        return LIBMAIX_ERR_NOT_IMPLEMENT;
    }

    libmaix_err_t libmaix_cv_image_crop(struct libmaix_image *src, int x, int y, int w, int h, struct libmaix_image **dst)
    {
        libmaix_err_t err = LIBMAIX_ERR_NONE;
        if(dst == NULL)
        {
            return LIBMAIX_ERR_PARAM;
        }
        if(src->width==0 || src->height==0 || src->data == NULL)
        {
            return LIBMAIX_ERR_PARAM;
        }

        int new_mem = libmaix_cv_image_load(src, dst);
        // -------------------------------
        LIBMAIX_IMAGE_ERROR(LIBMAIX_ERR_NOT_IMPLEMENT);
        // -------------------------------
        libmaix_cv_image_free(err, new_mem, dst);
        return LIBMAIX_ERR_NOT_IMPLEMENT;
    }

    libmaix_err_t libmaix_cv_image_rotate(libmaix_image_t *src, int rotate, libmaix_image_t **dst)
    {
        libmaix_err_t err = LIBMAIX_ERR_NONE;
        if(dst == NULL)
        {
            return LIBMAIX_ERR_PARAM;
        }
        if(src->width==0 || src->height==0 || src->data == NULL)
        {
            return LIBMAIX_ERR_PARAM;
        }

        int new_mem = libmaix_cv_image_load(src, dst);
        // -------------------------------
        LIBMAIX_IMAGE_ERROR(LIBMAIX_ERR_NOT_IMPLEMENT);
        // -------------------------------
        libmaix_cv_image_free(err, new_mem, dst);
        return LIBMAIX_ERR_NOT_IMPLEMENT;
    }

    libmaix_err_t libmaix_cv_image_test(libmaix_image_t *src, libmaix_image_t *dst)
    {
        if (dst->data == NULL && src->data == NULL && src->mode != dst->mode)
        {
            return LIBMAIX_ERR_PARAM;
        }

        if (src->mode != LIBMAIX_IMAGE_MODE_RGB888)
        {
            return LIBMAIX_ERR_NOT_IMPLEMENT;
        }

        // cv::Mat s(src->width, src->height, CV_8UC3, src->data);

        // cv::imwrite("/tmp/src.jpg", s);

        // cv::Mat input(src->width, src->height, CV_8UC3, const_cast<char *>((char *)src->data));

        // cv::Mat image = cv::imread("/home/res/logo.png", CV_LOAD_IMAGE_UNCHANGED);

        // if (!image.empty())
        // {
        //     mergeImage(input, image, cv::Point(5, 3));
        // }

        // cv::rectangle(input, cv::Rect(170, 50, 50, 50), cv::Scalar(0, 255, 0), 4);

        // cv::Mat tmp(src->width, src->height, CV_8UC3, src->data);

        // cv::imwrite("/tmp/tmp.jpg", tmp);

        // cv::putText(input, "abcdefg", cv::Point(50, 30), 2, 1.0f, CV_RGB(255, 0, 0));

        // cv::String text = u8"123测试asd的テスター";

        // int fontHeight = 20;
        // int thickness = -1;
        // int linestyle = 8;
        // int baseline = 0;

        // cv::Ptr<cv::freetype::FreeType2> ft2;
        // ft2 = cv::freetype::createFreeType2();
        // ft2->loadFontData("./txwzs.ttf", 0);

        // cv::Size textSize = ft2->getTextSize(text,
        //                                      fontHeight,
        //                                      thickness,
        //                                      &baseline);

        // if (thickness > 0)
        // {
        //     baseline += thickness;
        // }

        // cv::Point textOrg((input.cols - textSize.width) * 0.75, (input.rows + textSize.height) * 0.75);

        // cv::line(input, textOrg + cv::Point(0, thickness + 5),
        //          textOrg + cv::Point(textSize.width, thickness + 50),
        //          cv::Scalar(0, 0, 255), 1, 8);

        // ft2->putText(input, text, textOrg, fontHeight,
        //              cv::Scalar(0, 255, 0), thickness, linestyle, true);

        // int w = 240, h = 240, r = 40, a = 10, b = 20;
        // cv::Point center(w / 2, h / 2);

        // cv::circle(input, center, r, cv::Scalar(0, 0, 255), cv::FILLED);
        // cv::ellipse(input, center, cv::Size(a, b), 0, 0, 360, cv::Scalar(255, 0, 0));
        // cv::ellipse(input, center, cv::Size(a, b), 45, 0, 360, cv::Scalar(255, 0, 0));
        // cv::ellipse(input, center, cv::Size(a, b), -45, 0, 360, cv::Scalar(255, 0, 0));
        // cv::ellipse(input, center, cv::Size(a, b), 90, 0, 360, cv::Scalar(255, 0, 0));

        // const int numPts = 4;
        // cv::Point pts[numPts] = {cv::Point(10, 10), cv::Point(5, 30), cv::Point(35, 30), cv::Point(30, 10)};
        // const cv::Point *ppt[1] = {pts};
        // cv::fillPoly(input, ppt, &numPts, 1, cv::Scalar(0, 255, 0));

        // memcpy(dst->data, input.data, src->width * src->height * 3);

        return LIBMAIX_ERR_NONE;
    }
}