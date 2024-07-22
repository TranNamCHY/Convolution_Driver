/*
 * max_pooling.h
 *
 *  Created on: Mar 19, 2024
 *      Author: Truong
 */

#ifndef SRC_MAX_POOLING_H_
#define SRC_MAX_POOLING_H_


typedef struct Max_pooling_type {
    uint32_t Done;
    uint32_t Control;
    uint32_t Size_Paras;
} Max_pooling_type;

/*
    Byte arrangement of Done register
*/
#define MP_DONE (1u<<0)

/*
    Byte arrangement of Control register
*/
#define MP_RESET (1u<<0)
#define MP_START (1u<<1)

/*
    Byte arrangement of Size_Paras register
*/
#define MP_W_LOW 0
#define MP_H_LOW 8
#define MP_X_POOL_LOW 16
#define MP_Y_POOL_LOW 24

#endif /* SRC_MAX_POOLING_H_ */