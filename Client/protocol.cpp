// PDU 构造实现：一次 malloc 分配“头部 + 柔性数组”，全部字段清零。
// caData 也清零，保证未使用的定长参数为 0（对端可依赖该默认值做版本兼容判断）。
#include "protocol.h"
#include <stdlib.h>
#include <string.h>
//构造一个PDU对象并且返回其地址， 传参：柔性数组大小
// PDU 内存管理规则：
// 1. 所有 PDU 必须由分配该 PDU 的同一进程内配套函数释放
//    - Client: mkPDU() 用 malloc → 必须用 free() 释放
//    - Server: mkPDU() 用 memalloc → 必须用 memfree() 释放
// 2. 禁止跨进程/跨策略释放 PDU
// 3. 接收缓冲区中的原始字节不由 seedMsg() 释放
PDU *mkPDU(uint uiMsgLen)
{
    //计算PDU协议对象的总大小:sizeof(PDU)+柔性数组大小
    uint uitotallen=sizeof(PDU)+uiMsgLen;
    //利用malloc 分配 对应PDU协议对象的大小的内存
    PDU* pdu=(PDU*)malloc(uitotallen);
    //检查是否成功分配内存空间
    if(pdu==nullptr){
        exit(1);
    }
    //进行 PDU协议 对象 的内容构建
    memset(pdu,0,uitotallen);//全部初始化为0
    pdu->uiTotalLen=uitotallen;//赋值总大小
    pdu->uiMsgLen=uiMsgLen;//赋值柔性数组长度
    return pdu;//返回构建的PDU协议对象地址
}
