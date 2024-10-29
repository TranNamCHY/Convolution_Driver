// matrix_flat.c
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
void print_flat_matrix(float *matrix, int rows, int cols) {
    int i,j;
    for (i = 0; i < rows; i++) {
        for (j = 0; j < cols; j++) {
            printf("%f ", matrix[i * cols + j]);
        }
        printf("\n");
    }
}

int matrix_multiply(float *matrix1, int rows1, int cols1, float *matrix2, int rows2, int cols2, float* result_matrix){
    int i,j,k;
    if(cols1 != rows2){
        printf("Error: Cols1 and Rows1 not equal !");
        return -1;
    }
    float tempt = 0;
    for(i = 0; i < rows1; i++){
        for(j = 0; j < cols2; j++){
            tempt = 0;
            for(k = 0; k < cols1; k++){
                tempt += matrix1[i*cols1 + k] * matrix2[k*cols2 + j];
            }
            result_matrix[i*cols2 + j] = tempt;
        }
    }
    //print_flat_matrix(result_matrix,rows1, cols2);
    return 0;
}

void convolution_2d(float *source_buffer, int image_height, int image_width, float *kernel_buffer, float* result_matrix,char* activition,float test){
    //printf("got in conv2d c: ");
    int i,j,k,q;
    int dest_image_height = image_height - 2; 
    char* relu_is_active = strstr(activition, (char*)"relu");
    int dest_image_width = image_width - 2;
    float tempt = 0;
	for(i = 0; i < dest_image_height; i++){
		for(j = 0; j < dest_image_width; j++){
            result_matrix[i * dest_image_width + j] = 0;
            //printf("%.2f ",source_buffer[i*image_width + j]);
			for(k = 0; k < 3; k++){
				for(q = 0; q < 3; q++){
					result_matrix[i * dest_image_width + j] += kernel_buffer[k*3 + q] * source_buffer[((i) + k ) * image_width + (j) + q];
				}
			}
		}
	}
}


void convolution_2d_8bit(int8_t *source_buffer, int image_height, int image_width, int8_t *kernel_buffer, int32_t* result_matrix){
    int dest_image_height = image_height - 2; 
    int dest_image_width = image_width - 2;
    int i,j,k,q;
    float tempt = 0;
    //printf("Got in C func: %d",source_buffer[2]);
	for(i = 0; i < dest_image_height; i++){
		for(j = 0; j < dest_image_width; j++){
            result_matrix[i * dest_image_width + j] = 0;
			for(k = 0; k < 3; k++){
				for(q = 0; q < 3; q++){
					result_matrix[i * dest_image_width + j] += kernel_buffer[k*3 + q] * source_buffer[((i) + k ) * image_width + (j) + q];
				}
			}
		}
	}
}

void relu_activition(float* source_buffer, int image_height,int image_width,float* result_matrix, float max_value){
    //printf("Got value: %f",max_value);
    int i,j; 
    for(i=0;i<image_height;i++){
        for(j=0;j<image_width;j++){
            result_matrix[i * image_width + j] = 0;
            if(source_buffer[i * image_width + j] > 0){
                result_matrix[i * image_width + j] =  source_buffer[i*image_width + j];
            }
            if(source_buffer[i * image_width + j] > max_value){
                result_matrix[i * image_width + j] = max_value;
            }
        }
    }
}
void maxpool_2d(float *source_buffer, int image_height, int image_width, int input_image_num_channel, float* result_matrix,int pool_size, int* x_coordinate_matrix,int* y_coordinate_matrix){
    int dest_image_height = image_height/pool_size;
    int dest_image_width = image_width/pool_size;
    //int dest_image_height = image_height;
    //int dest_image_width = image_width;
    float tempt = -255;
    int i,j,k,q,c;
    int tempt_x = 0;
    int tempt_y = 0;
    /* for(i=0;i<dest_image_height;i++){
        for(j=0;j<dest_image_width;j++){
            result_matrix[i * dest_image_width + j] = 0;
            tempt = -255;
			for(k = 0; k < pool_size; k++){
				for(q = 0; q < pool_size; q++){
					if(source_buffer[((i*pool_size) + k ) * image_width + (j*pool_size) + q] > tempt){
                        tempt = source_buffer[((i*pool_size) + k ) * image_width + (j*pool_size) + q];
                    }
				}
			}
            result_matrix[i * dest_image_width + j] = tempt;
        }
    }  */
    for(c=0;c<input_image_num_channel;c++){
        //printf("Number %d channel: \n",c);
        for(i=0;i<dest_image_height;i++){
            //printf("[ ");
            for(j=0;j<dest_image_width;j++){
                result_matrix[c + i * input_image_num_channel * dest_image_width + j * input_image_num_channel] = 0;
                tempt = -255;
                //printf("%.2f ",source_buffer[c + i * input_image_num_channel * dest_image_width + j*input_image_num_channel]);
                for(k = 0; k < pool_size; k++){
				    for(q = 0; q < pool_size; q++){
                        if(source_buffer[c + (i*pool_size+k)*input_image_num_channel * image_width + (j*pool_size+q)*input_image_num_channel] > tempt){
                            tempt_x = i*pool_size+k;
                            tempt_y = j*pool_size+q;
                            tempt = source_buffer[c + (i*pool_size+k)*input_image_num_channel * image_width + (j*pool_size+q)*input_image_num_channel];
                        }
                        ///printf("idex: %d", c + (i*pool_size+k)*input_image_num_channel * image_width + (j*pool_size+q)*input_image_num_channel);
                        //printf("%.2f ",source_buffer[c + (i*pool_size+k)*input_image_num_channel * image_width + (j*pool_size+q)*input_image_num_channel]);
				    }
			    }
                result_matrix[c + i * input_image_num_channel * dest_image_width + j * input_image_num_channel] = tempt;
                x_coordinate_matrix[c + i * input_image_num_channel * dest_image_width + j * input_image_num_channel] = tempt_x;
                y_coordinate_matrix[c + i * input_image_num_channel * dest_image_width + j * input_image_num_channel] = tempt_y;
                //coordinate_matrix[c + i * input_image_num_channel * dest_image_width + j * input_image_num_channel + 1] = tempt_y;
            }
            //printf("] \n");   
        }
    }
}

void softmax_activition(float* source_buffer, int length, float* result_matrix){
    //printf("Got value: %f",max_value);
    int i;
    float sum = 0;
    for(i=0;i<length;i++){
        sum += exp(source_buffer[i]);
    }
    for(i=0;i<length;i++){
        result_matrix[i] = exp(source_buffer[i]);
        result_matrix[i] /= sum;
    }
}