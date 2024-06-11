#include "layer.h"
#include "net.h"

#include "opencv2/opencv.hpp"

#include <float.h>
#include <stdio.h>
#include <vector>
#include <iostream>

#define MAX_STRIDE 32

static const char* class_names[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
    "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
    "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
    "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush"
};

struct Object
{
	cv::Rect_<float> rect;
	int label;
	float prob;
};

static float softmax(
	const float* src,
	float* dst,
	int length
)
{
	float alpha = -FLT_MAX;
	for (int c = 0; c < length; c++)
	{
		float score = src[c];
		if (score > alpha)
		{
			alpha = score;
		}
	}

	float denominator = 0;
	float dis_sum = 0;
	for (int i = 0; i < length; ++i)
	{
		dst[i] = expf(src[i] - alpha);
		denominator += dst[i];
	}
	for (int i = 0; i < length; ++i)
	{
		dst[i] /= denominator;
		dis_sum += i * dst[i];
	}
	return dis_sum;
}
static float clamp(float val, float min = 0.f, float max = 1280.f){
	return val > min ? (val < max ? val : max) : min;
}

static void generate_proposals(
	int stride,
	const ncnn::Mat& feat_blob,
	const float prob_threshold,
	std::vector<Object>& objects
)
{
	const int reg_max = 16;
	float dst[16];
	const int num_w = feat_blob.w;
	const int num_grid_y = feat_blob.c;
	const int num_grid_x = feat_blob.h;

	const int num_class = num_w - 4 * reg_max;

	for (int i = 0; i < num_grid_y; i++)
	{
		for (int j = 0; j < num_grid_x; j++)
		{

			const float* matat = feat_blob.channel(i).row(j);

			int class_index = 0;
			float class_score = -FLT_MAX;
			for (int c = 0; c < num_class; c++)
			{
				float score = matat[c];
				if (score > class_score)
				{
					class_index = c;
					class_score = score;
				}
			}
			if (class_score >= prob_threshold)
			{

				float x0 = j + 0.5f - softmax(matat + num_class, dst, 16);
				float y0 = i + 0.5f - softmax(matat + num_class + 16, dst, 16);
				float x1 = j + 0.5f + softmax(matat + num_class + 2 * 16, dst, 16);
				float y1 = i + 0.5f + softmax(matat + num_class + 3 * 16, dst, 16);

				x0 *= stride;
				y0 *= stride;
				x1 *= stride;
				y1 *= stride;

				Object obj;
				obj.rect.x = x0;
				obj.rect.y = y0;
				obj.rect.width = x1 - x0;
				obj.rect.height = y1 - y0;
				obj.label = class_index;
				obj.prob = class_score;
				objects.push_back(obj);

			}
		}
	}
}


static int detect_yolov10(const cv::Mat& bgr, std::vector<Object>& objects)
{
	ncnn::Net yolov10;

	yolov10.opt.use_vulkan_compute = false;
	yolov10.opt.num_threads = 4;
	// yolov10.opt.use_bf16_storage = true;

	// original pretrained model from https://github.com/ultralytics/ultralytics
	// the ncnn model https://github.com/nihui/ncnn-assets/tree/master/models
	if (yolov10.load_param("./yolov10n.ncnn.param"))
		exit(-1);
	if (yolov10.load_model("./yolov10n.ncnn.bin"))
		exit(-1);

	const int target_size = 640;
	const float prob_threshold = 0.25f;

	int img_w = bgr.cols;
	int img_h = bgr.rows;

	// letterbox pad to multiple of MAX_STRIDE
	int w = img_w;
	int h = img_h;
	float scale = 1.f;
	if (w > h)
	{
		scale = (float)target_size / w;
		w = target_size;
		h = h * scale;
	}
	else
	{
		scale = (float)target_size / h;
		h = target_size;
		w = w * scale;
	}

	ncnn::Mat in = ncnn::Mat::from_pixels_resize(bgr.data, ncnn::Mat::PIXEL_BGR2RGB, img_w, img_h, w, h);

	// pad to target_size rectangle
	// ultralytics/yolo/data/dataloaders/v5augmentations.py letterbox
	// int wpad = (w + MAX_STRIDE - 1) / MAX_STRIDE * MAX_STRIDE - w;
	// int hpad = (h + MAX_STRIDE - 1) / MAX_STRIDE * MAX_STRIDE - h;

    int wpad = target_size - w;
    int hpad = target_size - h;

	int top = hpad / 2;
	int bottom = hpad - hpad / 2;
	int left = wpad / 2;
	int right = wpad - wpad / 2;

	ncnn::Mat in_pad;
	ncnn::copy_make_border(in,
		in_pad,
		top,
		bottom,
		left,
		right,
		ncnn::BORDER_CONSTANT,
		114.f);

	const float norm_vals[3] = { 1 / 255.f, 1 / 255.f, 1 / 255.f };
	in_pad.substract_mean_normalize(0, norm_vals);

	ncnn::Extractor ex = yolov10.create_extractor();

	ex.input("in0", in_pad);

	std::vector<Object> proposals;


	// stride 8
	{
		ncnn::Mat out;
		ex.extract("out0", out);

		std::vector<Object> objects8;
		generate_proposals(8, out, prob_threshold, objects8);

		proposals.insert(proposals.end(), objects8.begin(), objects8.end());
	}

	// stride 16
	{
		ncnn::Mat out;

		ex.extract("out1", out);

		std::vector<Object> objects16;
		generate_proposals(16, out, prob_threshold, objects16);

		proposals.insert(proposals.end(), objects16.begin(), objects16.end());
	}

	// stride 32
	{
		ncnn::Mat out;

		ex.extract("out2", out);

		std::vector<Object> objects32;
		generate_proposals(32, out, prob_threshold, objects32);

		proposals.insert(proposals.end(), objects32.begin(), objects32.end());
	}
    // objects = proposals;
    for (auto& pro : proposals)
	{
        float x0 = pro.rect.x;
		float y0 = pro.rect.y;
		float x1 = pro.rect.x + pro.rect.width;
		float y1 = pro.rect.y + pro.rect.height;
		float& score = pro.prob;
		int& label = pro.label;

		x0 = (x0 - (wpad / 2)) / scale;
		y0 = (y0 - (hpad / 2)) / scale;
		x1 = (x1 - (wpad / 2)) / scale;
		y1 = (y1 - (hpad / 2)) / scale;

		x0 = clamp(x0, 0.f, img_w);
		y0 = clamp(y0, 0.f, img_h);
		x1 = clamp(x1, 0.f, img_w);
		y1 = clamp(y1, 0.f, img_h);

		Object obj;
		obj.rect.x = x0;
		obj.rect.y = y0;
		obj.rect.width = x1 - x0;
		obj.rect.height = y1 - y0;
		obj.prob = score;
		obj.label = label;
		objects.push_back(obj);
	}
	// non_max_suppression(proposals, objects,
	// 	img_h, img_w, hpad / 2, wpad / 2,
	// 	scale, scale, prob_threshold, nms_threshold);
	return 0;
}

static void draw_objects(const cv::Mat& bgr, const std::vector<Object>& objects)
{
	for (size_t i = 0; i < objects.size(); i++){
		const Object& obj = objects[i];

//		fprintf(stderr, "%d = %.5f at %.2f %.2f %.2f x %.2f\n", obj.label, obj.prob,
//			obj.rect.x, obj.rect.y, obj.rect.width, obj.rect.height);
		cv::rectangle(bgr, obj.rect, cv::Scalar(255, 0, 0));

		char text[256];
		sprintf(text, "%s %.1f%%", class_names[obj.label], obj.prob * 100);

		int baseLine = 0;
		cv::Size label_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);

		int x = obj.rect.x;
		int y = obj.rect.y - label_size.height - baseLine;
		if (y < 0)
			y = 0;
		if (x + label_size.width > bgr.cols)
			x = bgr.cols - label_size.width;

		cv::rectangle(bgr, cv::Rect(cv::Point(x, y), cv::Size(label_size.width, label_size.height + baseLine)),
			cv::Scalar(255, 255, 255), -1);

		cv::putText(bgr, text, cv::Point(x, y + label_size.height),
			cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0));
	}
}

int main(int argc, char** argv)
{
	if (argc != 2){
		fprintf(stderr, "Usage: %s [imagepath]\n", argv[0]);
		return -1;
	}

	const char* imagepath = argv[1];

	cv::Mat m = cv::imread(imagepath, 1);
	if (m.empty()){
		fprintf(stderr, "cv::imread %s failed\n", imagepath);
		return -1;
	}
	std::vector<Object> objects;
	detect_yolov10(m, objects);
	draw_objects(m, objects);

    cv::imshow("image", m);
    cv::waitKey(0);
//    cv::imwrite("output.jpg", m);
	return 0;
}
