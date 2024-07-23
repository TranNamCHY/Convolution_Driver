/*
 * conv2d.h
 *
 *  Created on: Mar 11, 2024
 *      Author: Truong
 */

#ifndef SRC_CONV2D_H_
#define SRC_CONV2D_H_
/*
    Byte arrangement of Status register
*/
#define INPUTDMAC_DONE (1u << 0)
#define CONV2D_DONE (1u << 1)

/*
    Byte arrangement of Control register
*/
#define RESET           (1u << 0)
#define C_WE_IN         (1u << 1)
#define INPUTDMAC_START (1u << 2)
#define CONV2D_START    (1u << 3)

/*
    Byte arrangement of Address_and_Coeff register
*/
#define C_WA_IN_LOW 0
#define C_WA_IN_HIGHT 7
#define COEFF_IN_LOW 8
#define COEFF_IN_HIGH 15

/*
    Byte arrangement of BCR register
*/
#define B_LOW 0
#define B_HIGH 9
#define C_LOW 10
#define C_HIGH 19
#define R_LOW 20
#define R_HIGH 29

#define K_ADDR              0
#define K2_MINUS1_ADDR      1
#define K2_MINUSK_ADDR      2
#define WEIGHT_STR_ADDR     3

/*
    Some parameters may not change between Convolution layers:
        Iaddr, k, k2minus1, k2minusk
*/
#define K 3
#define K2_MINUS1 8
#define K2_MINUSK 6

/*
    Status register : read register
    Remaining        : write register
*/
typedef struct Con2D_Type {
    void* __iomem BaseAddress;
    uint32_t Status; // This is the actual base address
    uint32_t Control;
    uint32_t Address_and_Coeff;
    uint32_t BCR;
    uint32_t M2minus1;
    uint32_t IAddr;
} Con2D_Type;

typedef struct con2d_size_paras {
    uint16_t B;
    uint16_t C;
    uint16_t R;
    uint16_t M2minus1;
} con2d_size_paras;

typedef struct Image_type {
    void* __iomem BaseAddress;
	uint32_t Reset;
}Image_type;

#endif /* SRC_CONV2D_H_ */