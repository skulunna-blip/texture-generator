depth_anything_v2_small_fp16.onnx
===============================

What it is
----------
Depth Anything V2 (Small) exported to ONNX. The app runs it through ONNX
Runtime to estimate a depth map from the source photo. That depth map drives
the height map, and therefore the normal, ambient-occlusion and bump maps.

Why it is here at all: the classical pipeline derives height from luminance,
which cannot tell a painted pattern from real relief - a printed stripe and a
moulded ridge produce the same edge response. A depth model sees geometry, so
the relief it produces describes the surface instead of the paint. The
luminance detail is still used; the two are combined.

Licence and provenance
----------------------
Upstream project : https://github.com/DepthAnything/Depth-Anything-V2
Upstream model   : depth-anything/Depth-Anything-V2-Small
ONNX export      : https://huggingface.co/onnx-community/depth-anything-v2-small
Licence          : Apache-2.0 - full text in
                   LICENSE-DepthAnythingV2-Apache-2.0.txt
Copyright        : Lihe Yang, Bingyi Kang, Zilong Huang, Zhen Zhao,
                   Xiaogang Xu, Jiashi Feng, Hengshuang Zhao (2024)

Apache-2.0 permits commercial use, modification and redistribution provided
the licence text and attribution are kept with the distribution. Both travel
in this folder and in the portable package.

*** Do not swap in a larger checkpoint. ***
Upstream states plainly: "Depth-Anything-V2-Small model is under the
Apache-2.0 license. Depth-Anything-V2-Base/Large/Giant models are under the
CC-BY-NC-4.0 license." The Base, Large and Giant models are NON-COMMERCIAL.
Only Small may be redistributed with this application.

Citation
--------
@article{depth_anything_v2,
  title={Depth Anything V2},
  author={Yang, Lihe and Kang, Bingyi and Huang, Zilong and Zhao, Zhen and
          Xu, Xiaogang and Feng, Jiashi and Zhao, Hengshuang},
  journal={arXiv:2406.09414},
  year={2024}
}

Why fp16 rather than int8 or fp32
---------------------------------
Measured on CPU with the session reused and warmed up (see
`C:/tmp/depthref/compare.py` in the development notes):

           322 px   518 px   depth corr   gradient corr   load
  int8     1.16 s   3.12 s   0.9547       0.9490          0.78 s
  fp16     0.42 s   1.16 s   0.9996       0.9990          0.93 s
  fp32     0.38 s   0.94 s   -            -               0.45 s

int8 was both three times slower and measurably different - dynamic
quantisation's quantise/dequantise overhead dominates for a ViT this small.
fp16 holds a 0.9990 gradient correlation against fp32 (the gradient is what
actually feeds the normal and AO maps) at half the file size.
