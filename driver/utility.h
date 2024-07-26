#ifndef UTILITY_H_
#define UTILITY_H_
#include<linux/io.h>
#include<linux/types.h>
#include <linux/jiffies.h>
uint32_t read_register(void* base_adrr,unsigned int offset_reg){
    return readl(base_adrr + offset_reg);
}

int write_and_check_register(void * base_adrr,unsigned int offset_reg,uint32_t value,uint32_t mask,unsigned int timeout,int line_num){
    uint32_t tempt;
    unsigned long init_time;
    pr_info("For debug purpose: Write at: %x ,offset register: %d, Line: %d, first value: %x!",(uint32_t)base_adrr,offset_reg,line_num,read_register(base_adrr,offset_reg));
    tempt = read_register(base_adrr,offset_reg);
    tempt = (tempt&(~mask)) | (value&mask);
    writel(tempt,(void*)(base_adrr + offset_reg));
    init_time = jiffies;
    while(jiffies_to_msecs(jiffies - init_time)<= timeout){
        if((read_register(base_adrr,offset_reg)&mask)  == (value&mask))
            goto success;
    }
    pr_info("Error with  write func at line,: %d, got value: %x\n",line_num,read_register(base_adrr,offset_reg));
    return -1;
success:
    return 0;
}
#endif /* UTILITY_H_ */