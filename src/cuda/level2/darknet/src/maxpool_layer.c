#include "maxpool_layer.h"
#include "cuda.h"
#include <stdio.h>

void test_maxpool_layer_forward(int batch, int height, int width, int chan,
                int size, int stride, int padding) {
    printf("----- maxpool forward -----\n");
    maxpool_layer l = make_maxpool_layer(batch, height, width, chan, size, stride, padding);
    network *net = make_network(1);
    net->input_gpu = cuda_make_array(NULL, l.batch*l.w*l.h*l.c);
    forward_maxpool_layer_gpu(l, *net);
    free_layer(l);
    free_network(net);
    printf("---------------------------\n\n");
}

void test_maxpool_layer_backward(int batch, int height, int width, int chan,
                int size, int stride, int padding) {
    printf("----- maxpool backward -----\n");
    maxpool_layer l = make_maxpool_layer(batch, height, width, chan, size, stride, padding);
    network *net = make_network(1);
    net->delta_gpu = cuda_make_array(NULL, l.batch*l.h*l.w*l.c);
    net->input_gpu = cuda_make_array(NULL, l.batch*l.w*l.h*l.c);
    backward_maxpool_layer_gpu(l, *net);
    free_layer(l);
    free_network(net);
    printf("----------------------------\n\n");
}

image get_maxpool_image(maxpool_layer l)
{
    int h = l.out_h;
    int w = l.out_w;
    int c = l.c;
    return float_to_image(w,h,c,l.output);
}

image get_maxpool_delta(maxpool_layer l)
{
    int h = l.out_h;
    int w = l.out_w;
    int c = l.c;
    return float_to_image(w,h,c,l.delta);
}

maxpool_layer make_maxpool_layer(int batch, int h, int w, int c, int size, int stride, int padding)
{
    maxpool_layer l = {0};
    l.type = MAXPOOL;
    l.batch = batch;
    l.h = h;
    l.w = w;
    l.c = c;
    l.pad = padding;
    l.out_w = (w + padding - size)/stride + 1;
    l.out_h = (h + padding - size)/stride + 1;
    l.out_c = c;
    l.outputs = l.out_h * l.out_w * l.out_c;
    l.inputs = h*w*c;
    l.size = size;
    l.stride = stride;
    int output_size = l.out_h * l.out_w * l.out_c * batch;
    l.indexes = calloc(output_size, sizeof(int));
    l.output =  calloc(output_size, sizeof(float));
    l.delta =   calloc(output_size, sizeof(float));
    l.forward = forward_maxpool_layer;
    l.backward = backward_maxpool_layer;
    #ifdef GPU
    l.forward_gpu = forward_maxpool_layer_gpu;
    l.backward_gpu = backward_maxpool_layer_gpu;
    l.indexes_gpu = cuda_make_int_array(0, output_size);
    l.output_gpu  = cuda_make_array(l.output, output_size);
    l.delta_gpu   = cuda_make_array(l.delta, output_size);

#ifdef CUDNN
    cudnnStatus_t stat = cudnnCreatePoolingDescriptor(&l.poolingDesc);
    assert(stat == CUDNN_STATUS_SUCCESS);
    // no padding for now
    stat = cudnnSetPooling2dDescriptor(l.poolingDesc, CUDNN_POOLING_MAX,
            CUDNN_NOT_PROPAGATE_NAN, l.h, l.w, l.pad, l.pad, l.stride, l.stride);
    assert(stat == CUDNN_STATUS_SUCCESS);

    stat = cudnnCreateTensorDescriptor(&l.poolingInputTensorDesc);
    assert(stat == CUDNN_STATUS_SUCCESS);

    stat = cudnnSetTensor4dDescriptor(l.poolingInputTensorDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT,
            l.batch, l.c, l.h, l.w);
    assert(stat == CUDNN_STATUS_SUCCESS);

    stat = cudnnCreateTensorDescriptor(&l.poolingOutputTensorDesc);
    assert(stat == CUDNN_STATUS_SUCCESS);

    stat = cudnnSetTensor4dDescriptor(l.poolingOutputTensorDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT,
            l.batch, l.out_c, l.out_h, l.out_w);
    assert(stat == CUDNN_STATUS_SUCCESS);


#endif
    #endif
    fprintf(stderr, "max          %d x %d / %d  %4d x%4d x%4d   ->  %4d x%4d x%4d\n", size, size, stride, w, h, c, l.out_w, l.out_h, l.out_c);
    return l;
}

void resize_maxpool_layer(maxpool_layer *l, int w, int h)
{
    l->h = h;
    l->w = w;
    l->inputs = h*w*l->c;

    l->out_w = (w + l->pad - l->size)/l->stride + 1;
    l->out_h = (h + l->pad - l->size)/l->stride + 1;
    l->outputs = l->out_w * l->out_h * l->c;
    int output_size = l->outputs * l->batch;

    l->indexes = realloc(l->indexes, output_size * sizeof(int));
    l->output = realloc(l->output, output_size * sizeof(float));
    l->delta = realloc(l->delta, output_size * sizeof(float));

    #ifdef GPU
    cuda_free((float *)l->indexes_gpu);
    cuda_free(l->output_gpu);
    cuda_free(l->delta_gpu);
    l->indexes_gpu = cuda_make_int_array(0, output_size);
    l->output_gpu  = cuda_make_array(l->output, output_size);
    l->delta_gpu   = cuda_make_array(l->delta,  output_size);
    #endif
}

void forward_maxpool_layer(const maxpool_layer l, network net)
{
    int b,i,j,k,m,n;
    int w_offset = -l.pad/2;
    int h_offset = -l.pad/2;

    int h = l.out_h;
    int w = l.out_w;
    int c = l.c;

    for(b = 0; b < l.batch; ++b){
        for(k = 0; k < c; ++k){
            for(i = 0; i < h; ++i){
                for(j = 0; j < w; ++j){
                    int out_index = j + w*(i + h*(k + c*b));
                    float max = -FLT_MAX;
                    int max_i = -1;
                    for(n = 0; n < l.size; ++n){
                        for(m = 0; m < l.size; ++m){
                            int cur_h = h_offset + i*l.stride + n;
                            int cur_w = w_offset + j*l.stride + m;
                            int index = cur_w + l.w*(cur_h + l.h*(k + b*l.c));
                            int valid = (cur_h >= 0 && cur_h < l.h &&
                                         cur_w >= 0 && cur_w < l.w);
                            float val = (valid != 0) ? net.input[index] : -FLT_MAX;
                            max_i = (val > max) ? index : max_i;
                            max   = (val > max) ? val   : max;
                        }
                    }
                    l.output[out_index] = max;
                    l.indexes[out_index] = max_i;
                }
            }
        }
    }
}

void backward_maxpool_layer(const maxpool_layer l, network net)
{
    int i;
    int h = l.out_h;
    int w = l.out_w;
    int c = l.c;
    for(i = 0; i < h*w*c*l.batch; ++i){
        int index = l.indexes[i];
        net.delta[index] += l.delta[i];
    }
}
