#include "protocol.h"
#include <stdlib.h>
#include <string.h>
#include "memalloc.h"

PDU *mkPDU(uint uiMsgLen)
{
    uint uitotallen = sizeof(PDU) + uiMsgLen;
    PDU* pdu = (PDU*)memalloc(uitotallen);
    if(pdu == nullptr){
        exit(1);
    }
    memset(pdu, 0, uitotallen);
    pdu->uiTotalLen = uitotallen;
    pdu->uiMsgLen = uiMsgLen;
    return pdu;
}
