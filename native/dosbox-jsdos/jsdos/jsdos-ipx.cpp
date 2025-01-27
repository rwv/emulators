//
// Created by caiiiycuk on 30.09.2021.
//

#include "dosbox.h"
#include <cassert>
#include <cstdio>
#include <sys/time.h>
#include <jsdos-asyncify.h>

#if C_IPX

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <cstdlib>

#include "SDL_net.h"
#include "callback.h"
#include "cpu.h"
#include "cross.h"
#include "debug.h"
#include "dos_system.h"
#include "inout.h"
#include "ipx.h"
#include "ipxserver.h"
#include "mem.h"
#include "pic.h"
#include "programs.h"
#include "protocol.h"
#include "regs.h"
#include "setup.h"
#include "support.h"
#include "timer.h"

#define SOCKTABLESIZE	150 // DOS IPX driver was limited to 150 open sockets

extern int SDLnet_useCallbackIdle;

struct ipxnetaddr {
  Uint8 netnum[4];   // Both are big endian
  Uint8 netnode[6];
} localIpxAddr;

Bit32u udpPort;
IPaddress ipxServConnIp;			// IPAddress for client connection to server
TCPsocket ipxClientSocket;
Bit8u recvBuffer[IPXBUFFERSIZE];	// Incoming packet buffer

static RealPt ipx_callback;

packetBuffer incomingPacket;

static Bit16u socketCount;
static Bit16u opensockets[SOCKTABLESIZE];

namespace {
long int now() {
  struct timeval tp;
  gettimeofday(&tp, NULL);
  return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}
}

static Bit16u swapByte(Bit16u sockNum) {
  return (((sockNum>> 8)) | (sockNum << 8));
}

void UnpackIP(PackedIP ipPack, IPaddress * ipAddr) {
  ipAddr->host = ipPack.host;
  ipAddr->port = ipPack.port;
}

void PackIP(IPaddress ipAddr, PackedIP *ipPack) {
  ipPack->host = ipAddr.host;
  ipPack->port = ipAddr.port;
}

IPXHeader* readNextIPXHeader(TCPsocket clientSocket) {
  static Uint8 buffer[IPXBUFFERSIZE];
  int available = SDLNet_TCP_Recv(clientSocket, buffer, 4); // read up to length
  if (available <= 0) {
    return nullptr;
  }

  while (available < 4) {
    available += SDLNet_TCP_Recv(clientSocket, buffer + available, 4 - available);
    if (available < 4) {
      CALLBACK_Idle();
    }
  }

  auto header = (IPXHeader*) buffer;
  auto length = SDLNet_Read16(header->length);
  while (available < length) {
    available += SDLNet_TCP_Recv(clientSocket, buffer + available, length - available);
    if (available < length) {
      CALLBACK_Idle();
    }
  }

  return header;
}

ECBClass *ECBList;  // Linked list of ECB's
ECBClass* ESRList;	// ECBs waiting to be ESR notified

#ifdef IPX_DEBUGMSG
Bitu ECBSerialNumber = 0;
Bitu ECBAmount = 0;
#endif


ECBClass::ECBClass(Bit16u segment, Bit16u offset) {
  ECBAddr = RealMake(segment, offset);
  databuffer = 0;

#ifdef IPX_DEBUGMSG
  SerialNumber = ECBSerialNumber;
  ECBSerialNumber++;
  ECBAmount++;

  LOG_IPX("ECB: SN%7d created.   Number of ECBs: %3d, ESR %4x:%4x, ECB %4x:%4x",
          SerialNumber,ECBAmount,
          real_readw(RealSeg(ECBAddr),
                     RealOff(ECBAddr)+6),
          real_readw(RealSeg(ECBAddr),
                     RealOff(ECBAddr)+4),segment,offset);
#endif
  isInESRList = false;
  prevECB = NULL;
  nextECB = NULL;

  if (ECBList == NULL)
    ECBList = this;
  else {
    // Transverse the list until we hit the end
    ECBClass *useECB = ECBList;

    while(useECB->nextECB != NULL)
      useECB = useECB->nextECB;

    useECB->nextECB = this;
    this->prevECB = useECB;
  }

  iuflag = getInUseFlag();
  mysocket = getSocket();
}
void ECBClass::writeDataBuffer(Bit8u* buffer, Bit16u length) {
  if(databuffer!=0) delete [] databuffer;
  databuffer = new Bit8u[length];
  memcpy(databuffer,buffer,length);
  buflen=length;

}
bool ECBClass::writeData() {
  Bitu length=buflen;
  Bit8u* buffer = databuffer;
  fragmentDescriptor tmpFrag;
  setInUseFlag(USEFLAG_AVAILABLE);
  Bitu fragCount = getFragCount();
  Bitu bufoffset = 0;
  for(Bitu i = 0;i < fragCount;i++) {
    getFragDesc(i,&tmpFrag);
    for(Bitu t = 0;t < tmpFrag.size;t++) {
      real_writeb(tmpFrag.segment, tmpFrag.offset + t, buffer[bufoffset]);
      bufoffset++;
      if(bufoffset >= length) {
        setCompletionFlag(COMP_SUCCESS);
        setImmAddress(&buffer[22]);  // Write in source node
        return true;
      }
    }
  }
  if(bufoffset < length) {
    setCompletionFlag(COMP_MALFORMED);
    return false;
  }
  return false;
}

Bit16u ECBClass::getSocket(void) {
  return swapByte(real_readw(RealSeg(ECBAddr), RealOff(ECBAddr) + 0xa));
}

Bit8u ECBClass::getInUseFlag(void) {
  return real_readb(RealSeg(ECBAddr), RealOff(ECBAddr) + 0x8);
}

void ECBClass::setInUseFlag(Bit8u flagval) {
  iuflag = flagval;
  real_writeb(RealSeg(ECBAddr), RealOff(ECBAddr) + 0x8, flagval);
}

void ECBClass::setCompletionFlag(Bit8u flagval) {
  real_writeb(RealSeg(ECBAddr), RealOff(ECBAddr) + 0x9, flagval);
}

Bit16u ECBClass::getFragCount(void) {
  return real_readw(RealSeg(ECBAddr), RealOff(ECBAddr) + 34);
}

void ECBClass::getFragDesc(Bit16u descNum, fragmentDescriptor *fragDesc) {
  Bit16u memoff = RealOff(ECBAddr) + 30 + ((descNum+1) * 6);
  fragDesc->offset = real_readw(RealSeg(ECBAddr), memoff);
  memoff += 2;
  fragDesc->segment = real_readw(RealSeg(ECBAddr), memoff);
  memoff += 2;
  fragDesc->size = real_readw(RealSeg(ECBAddr), memoff);
}

RealPt ECBClass::getESRAddr(void) {
  return RealMake(real_readw(RealSeg(ECBAddr),
                             RealOff(ECBAddr)+6),
                  real_readw(RealSeg(ECBAddr),
                             RealOff(ECBAddr)+4));
}

void ECBClass::NotifyESR(void) {
  Bit32u ESRval = real_readd(RealSeg(ECBAddr), RealOff(ECBAddr)+4);
  if(ESRval || databuffer) { // databuffer: write data at realmode/v86 time
    // LOG_IPX("ECB: SN%7d to be notified.", SerialNumber);
    // take the ECB out of the current list
    if(prevECB == NULL) {	// was the first in the list
      ECBList = nextECB;
      if(ECBList != NULL) ECBList->prevECB = NULL;
    } else {		// not the first
      prevECB->nextECB = nextECB;
      if(nextECB != NULL) nextECB->prevECB = prevECB;
    }

    nextECB = NULL;
    // put it to the notification queue
    if(ESRList==NULL) {
      ESRList = this;
      prevECB = NULL;
    } else  {// put to end of ESR list
      ECBClass* useECB = ESRList;

      while(useECB->nextECB != NULL)
        useECB = useECB->nextECB;

      useECB->nextECB = this;
      prevECB = useECB;
    }
    isInESRList = true;
    PIC_ActivateIRQ(11);
  }
    // this one does not want to be notified, delete it right away
  else delete this;
}

void ECBClass::setImmAddress(Bit8u *immAddr) {
  for(Bitu i=0;i<6;i++)
    real_writeb(RealSeg(ECBAddr), RealOff(ECBAddr)+28+i, immAddr[i]);
}

void ECBClass::getImmAddress(Bit8u* immAddr) {
  for(Bitu i=0;i<6;i++)
    immAddr[i] = real_readb(RealSeg(ECBAddr), RealOff(ECBAddr)+28+i);
}

ECBClass::~ECBClass() {
#ifdef IPX_DEBUGMSG
  ECBAmount--;
  LOG_IPX("ECB: SN%7d destroyed. Remaining ECBs: %3d", SerialNumber,ECBAmount);
#endif

  if(isInESRList) {
    // in ESR list, always the first element is deleted.
    ESRList=nextECB;
  } else {
    if(prevECB == NULL) {	// was the first in the list
      ECBList = nextECB;
      if(ECBList != NULL) ECBList->prevECB = NULL;
    } else {	// not the first
      prevECB->nextECB = nextECB;
      if(nextECB != NULL) nextECB->prevECB = prevECB;
    }
  }
  if(databuffer!=0) delete [] databuffer;
}



static bool sockInUse(Bit16u sockNum) {
  for(Bitu i=0;i<socketCount;i++) {
    if (opensockets[i] == sockNum) return true;
  }
  return false;
}

static void OpenSocket(void) {
  Bit16u sockNum, sockAlloc;
  sockNum = swapByte(reg_dx);

  if(socketCount >= SOCKTABLESIZE) {
    reg_al = 0xfe; // Socket table full
    return;
  }

  if(sockNum == 0x0000) {
    // Dynamic socket allocation
    sockAlloc = 0x4002;
    while(sockInUse(sockAlloc) && (sockAlloc < 0x7fff)) sockAlloc++;
    if(sockAlloc > 0x7fff) {
      // I have no idea how this could happen if the IPX driver
      // is limited to 150 open sockets at a time
      LOG_ERR("IPX: Out of dynamic sockets");
    }
    sockNum = sockAlloc;
  } else {
    if(sockInUse(sockNum)) {
      reg_al = 0xff; // Socket already open
      return;
    }
  }

  opensockets[socketCount] = sockNum;
  socketCount++;

  reg_al = 0x00; // Success
  reg_dx = swapByte(sockNum);  // Convert back to big-endian
}

static void CloseSocket(void) {
  Bit16u sockNum, i;
  ECBClass* tmpECB = ECBList;
  ECBClass* tmp2ECB = ECBList;

  sockNum = swapByte(reg_dx);
  if(!sockInUse(sockNum)) return;

  for(i=0;i<socketCount-1;i++) {
    if (opensockets[i] == sockNum) {
      // Realign list of open sockets
      memcpy(&opensockets[i], &opensockets[i+1], SOCKTABLESIZE - (i + 1));
      break;
    }
  }
  --socketCount;

  // delete all ECBs of that socket
  while(tmpECB!=0) {
    tmp2ECB = tmpECB->nextECB;
    if(tmpECB->getSocket()==sockNum) {
      tmpECB->setCompletionFlag(COMP_CANCELLED);
      tmpECB->setInUseFlag(USEFLAG_AVAILABLE);
      delete tmpECB;
    }
    tmpECB = tmp2ECB;
  }
}

//static RealPt IPXVERpointer;

static bool IPX_Multiplex(void) {
  if(reg_ax != 0x7a00) return false;
  reg_al = 0xff;
  SegSet16(es,RealSeg(ipx_callback));
  reg_di = RealOff(ipx_callback);

  //reg_bx = RealOff(IPXVERpointer);
  //reg_cx = RealSeg(ipx_callback);
  return true;
}

static void IPX_AES_EventHandler(Bitu param)
{
  ECBClass* tmpECB = ECBList;
  ECBClass* tmp2ECB;
  while(tmpECB!=0) {
    tmp2ECB = tmpECB->nextECB;
    if(tmpECB->iuflag==USEFLAG_AESCOUNT && param==(Bitu)tmpECB->ECBAddr) {
      tmpECB->setCompletionFlag(COMP_SUCCESS);
      tmpECB->setInUseFlag(USEFLAG_AVAILABLE);
      tmpECB->NotifyESR();
      // LOG_IPX("AES Notification: ECB S/N %d",tmpECB->SerialNumber);
      return;
    }
    tmpECB = tmp2ECB;
  }
  LOG_ERR("!!!! Rouge AES !!!!" );
}

static void sendPacket(ECBClass* sendecb);

static void handleIpxRequest(void) {
  ECBClass *tmpECB;

  switch (reg_bx) {
    case 0x0000:	// Open socket
      OpenSocket();
      LOG_IPX("IPX: Open socket %4x", swapByte(reg_dx));
      break;
    case 0x0001:	// Close socket
      LOG_IPX("IPX: Close socket %4x", swapByte(reg_dx));
      CloseSocket();
      break;
    case 0x0002:	// get local target
      // es:si
      // Currently no support for multiple networks

      for(Bitu i = 0; i < 6; i++)
        real_writeb(SegValue(es),reg_di+i,real_readb(SegValue(es),reg_si+i+4));

      reg_cx=1;		// time ticks expected
      reg_al=0x00;	//success
      break;

    case 0x0003:		// Send packet
      tmpECB = new ECBClass(SegValue(es),reg_si);
      if(!incomingPacket.connected) {
        tmpECB->setInUseFlag(USEFLAG_AVAILABLE);
        tmpECB->setCompletionFlag(COMP_UNDELIVERABLE);
        delete tmpECB;	// not notify?
        reg_al = 0xff; // Failure
      } else {
        tmpECB->setInUseFlag(USEFLAG_SENDING);
        //LOG_IPX("IPX: Sending packet on %4x", tmpECB->getSocket());
        reg_al = 0x00; // Success
        sendPacket(tmpECB);
      }

      break;
    case 0x0004:  // Listen for packet
      tmpECB = new ECBClass(SegValue(es),reg_si);
      // LOG_IPX("ECB: SN%7d RECEIVE.", tmpECB->SerialNumber);
      if(!sockInUse(tmpECB->getSocket())) {  // Socket is not open
        reg_al = 0xff;
        tmpECB->setInUseFlag(USEFLAG_AVAILABLE);
        tmpECB->setCompletionFlag(COMP_HARDWAREERROR);
        delete tmpECB;
      } else {
        reg_al = 0x00;  // Success
        tmpECB->setInUseFlag(USEFLAG_LISTENING);
        /*LOG_IPX("IPX: Listen for packet on 0x%4x - ESR address %4x:%4x",
                tmpECB->getSocket(),
                RealSeg(tmpECB->getESRAddr()),
                RealOff(tmpECB->getESRAddr()));*/
      }
      break;

    case 0x0005:	// SCHEDULE IPX EVENT
    case 0x0007:	// SCHEDULE SPECIAL IPX EVENT
    {
      tmpECB = new ECBClass(SegValue(es),reg_si);
      // LOG_IPX("ECB: SN%7d AES. T=%fms.", tmpECB->SerialNumber,
      //	(1000.0f/(1193182.0f/65536.0f))*(float)reg_ax);
      PIC_AddEvent(IPX_AES_EventHandler,
                   (1000.0f/(1193182.0f/65536.0f))*(float)reg_ax,(Bitu)tmpECB->ECBAddr);
      tmpECB->setInUseFlag(USEFLAG_AESCOUNT);
      break;
    }
    case 0x0006:	// cancel operation
    {
      RealPt ecbaddress = RealMake(SegValue(es),reg_si);
      ECBClass* tmpECB= ECBList;
      ECBClass* tmp2ECB;
      while(tmpECB) {
        tmp2ECB=tmpECB->nextECB;
        if(tmpECB->ECBAddr == ecbaddress) {
          if(tmpECB->getInUseFlag()==USEFLAG_AESCOUNT)
            PIC_RemoveSpecificEvents(IPX_AES_EventHandler,(Bitu)ecbaddress);
          tmpECB->setInUseFlag(USEFLAG_AVAILABLE);
          tmpECB->setCompletionFlag(COMP_CANCELLED);
          delete tmpECB;
          reg_al=0;	// Success
          LOG_IPX("IPX: ECB canceled.");
          return;
        }
        tmpECB=tmp2ECB;
      }
      reg_al=0xff;	// Fail
      break;
    }
    case 0x0008:		// Get interval marker
      reg_ax = mem_readw(0x46c); // BIOS_TIMER
      break;
    case 0x0009:		// Get internetwork address
    {
      LOG_IPX("IPX: Get internetwork address %2x:%2x:%2x:%2x:%2x:%2x",
              localIpxAddr.netnode[5], localIpxAddr.netnode[4],
              localIpxAddr.netnode[3], localIpxAddr.netnode[2],
              localIpxAddr.netnode[1], localIpxAddr.netnode[0]);

      Bit8u * addrptr = (Bit8u *)&localIpxAddr;
      for(Bit16u i=0;i<10;i++)
        real_writeb(SegValue(es),reg_si+i,addrptr[i]);
      break;
    }
    case 0x000a:		// Relinquish control
      break;			// Idle thingy

    case 0x000b:		// Disconnect from Target
      break;			// We don't even connect

    case 0x000d:		// get packet size
      reg_cx=0;		// retry count
      reg_ax=1024;	// real implementation returns 1024
      break;

    case 0x0010:		// SPX install check
      reg_al=0;		// SPX not installed
      break;

    case 0x001a:		// get driver maximum packet size
      reg_cx=0;		// retry count
      reg_ax=IPXBUFFERSIZE;	// max packet size: something near the
      // ethernet packet size
      break;

    default:
      LOG_ERR("Unhandled IPX function: %4x", reg_bx);
      break;
  }
}

// Entrypoint handler
Bitu IPX_Handler(void) {
  handleIpxRequest();
  return CBRET_NONE;
}

// INT 7A handler
Bitu IPX_IntHandler(void) {
  handleIpxRequest();
  return CBRET_NONE;
}

static void pingAck(IPaddress retAddr) {
  IPXHeader regHeader;
  Bits result;

  SDLNet_Write16(0xffff, regHeader.checkSum);
  SDLNet_Write16(sizeof(regHeader), regHeader.length);

  SDLNet_Write32(0, regHeader.dest.network);
  PackIP(retAddr, &regHeader.dest.addr.byIP);
  SDLNet_Write16(0x2, regHeader.dest.socket);

  SDLNet_Write32(0, regHeader.src.network);
  memcpy(regHeader.src.addr.byNode.node, localIpxAddr.netnode, sizeof(regHeader.src.addr.byNode.node));
  SDLNet_Write16(0x2, regHeader.src.socket);
  regHeader.transControl = 0;
  regHeader.pType = 0x0;

  result = SDLNet_TCP_Send(ipxClientSocket, &regHeader, sizeof(regHeader));
}

static void pingSend(void) {
  IPXHeader regHeader;
  Bits result;

  SDLNet_Write16(0xffff, regHeader.checkSum);
  SDLNet_Write16(sizeof(regHeader), regHeader.length);

  SDLNet_Write32(0, regHeader.dest.network);
  regHeader.dest.addr.byIP.host = 0xffffffff;
  regHeader.dest.addr.byIP.port = 0xffff;
  SDLNet_Write16(0x2, regHeader.dest.socket);

  SDLNet_Write32(0, regHeader.src.network);
  memcpy(regHeader.src.addr.byNode.node, localIpxAddr.netnode, sizeof(regHeader.src.addr.byNode.node));
  SDLNet_Write16(0x2, regHeader.src.socket);
  regHeader.transControl = 0;
  regHeader.pType = 0x0;

  result = SDLNet_TCP_Send(ipxClientSocket, &regHeader, sizeof(regHeader));
  if(!result) {
    LOG_ERR("IPX: SDLNet_UDP_Send: %s\n", SDLNet_GetError());
  }
}

static void receivePacket(Bit8u *buffer, Bit16s bufSize) {
  ECBClass *useECB;
  ECBClass *nextECB;
  Bit16u *bufword = (Bit16u *)buffer;
  Bit16u useSocket = swapByte(bufword[8]);
  IPXHeader * tmpHeader;
  tmpHeader = (IPXHeader *)buffer;

  // Check to see if ping packet
  if(useSocket == 0x2) {
    // Is this a broadcast?
    if((tmpHeader->dest.addr.byIP.host == 0xffffffff) &&
       (tmpHeader->dest.addr.byIP.port == 0xffff)) {
      // Yes.  We should return the ping back to the sender
      IPaddress tmpAddr;
      UnpackIP(tmpHeader->src.addr.byIP, &tmpAddr);
      pingAck(tmpAddr);
      return;
    }
  }

  useECB = ECBList;
  while(useECB != NULL)
  {
    nextECB = useECB->nextECB;
    if(useECB->iuflag == USEFLAG_LISTENING && useECB->mysocket == useSocket) {
      useECB->writeDataBuffer(buffer, bufSize);
      useECB->NotifyESR();
      return;
    }
    useECB = nextECB;
  }
  LOG_IPX("IPX: RX Packet loss!");
}

static void IPX_ClientLoop(void) {
  static bool reentranceLock = false;
  if (reentranceLock) {
    return;
  }

  reentranceLock = true;
  IPXHeader* next;
  while (true) {
    next = readNextIPXHeader(ipxClientSocket);
    if (next == nullptr) {
      break;
    }
    receivePacket((Bit8u*) next, SDLNet_Read16(next->length));
  }
  reentranceLock = false;
}


void DisconnectFromServer(bool unexpected) {
  if(unexpected) LOG_ERR("IPX: Server disconnected unexpectedly");
  if(incomingPacket.connected) {
    incomingPacket.connected = false;
    TIMER_DelTickHandler(&IPX_ClientLoop);
    SDLNet_TCP_Close(ipxClientSocket);
  }
  client_network_disconnected(NETWORK_DOSBOX_IPX);
}

static void sendPacket(ECBClass* sendecb) {
  Bit8u outbuffer[IPXBUFFERSIZE];
  fragmentDescriptor tmpFrag;
  Bit16u i, fragCount,t;
  Bit16s packetsize;
  Bit16u *wordptr;
  Bits result;

  sendecb->setInUseFlag(USEFLAG_AVAILABLE);
  packetsize = 0;
  fragCount = sendecb->getFragCount();
  for(i=0;i<fragCount;i++) {
    sendecb->getFragDesc(i,&tmpFrag);
    if(i==0) {
      // Fragment containing IPX header
      // Must put source address into header
      Bit8u * addrptr;

      // source netnum
      addrptr = (Bit8u *)&localIpxAddr.netnum;
      for(Bit16u m=0;m<4;m++) {
        real_writeb(tmpFrag.segment,tmpFrag.offset+m+18,addrptr[m]);
      }
      // source node number
      addrptr = (Bit8u *)&localIpxAddr.netnode;
      for(Bit16u m=0;m<6;m++) {
        real_writeb(tmpFrag.segment,tmpFrag.offset+m+22,addrptr[m]);
      }
      // Source socket
      real_writew(tmpFrag.segment,tmpFrag.offset+28, swapByte(sendecb->getSocket()));

      // blank checksum
      real_writew(tmpFrag.segment,tmpFrag.offset, 0xffff);
    }

    for(t=0;t<tmpFrag.size;t++) {
      outbuffer[packetsize] = real_readb(tmpFrag.segment, tmpFrag.offset + t);
      packetsize++;
      if(packetsize>=IPXBUFFERSIZE) {
        LOG_ERR("IPX: Packet size to be sent greater than %d bytes.", IPXBUFFERSIZE);
        sendecb->setCompletionFlag(COMP_UNDELIVERABLE);
        sendecb->NotifyESR();
        return;
      }
    }
  }

  // Add length and source socket to IPX header
  wordptr = (Bit16u *)&outbuffer[0];
  // Blank CRC
  //wordptr[0] = 0xffff;
  // Length
  wordptr[1] = swapByte(packetsize);
  // Source socket
  //wordptr[14] = swapByte(sendecb->getSocket());

  sendecb->getFragDesc(0,&tmpFrag);
  real_writew(tmpFrag.segment,tmpFrag.offset+2, swapByte(packetsize));


  Bit8u immedAddr[6];
  sendecb->getImmAddress(immedAddr);
  // filter out broadcasts and local loopbacks
  // Real implementation uses the ImmedAddr to check wether this is a broadcast

  bool islocalbroadcast=true;
  bool isloopback=true;

  Bit8u * addrptr;

  addrptr = (Bit8u *)&localIpxAddr.netnum;
  for(Bitu m=0;m<4;m++) {
    if(addrptr[m]!=outbuffer[m+0x6])isloopback=false;
  }
  addrptr = (Bit8u *)&localIpxAddr.netnode;
  for(Bitu m=0;m<6;m++) {
    if(addrptr[m]!=outbuffer[m+0xa])isloopback=false;
    if(immedAddr[m]!=0xff) islocalbroadcast=false;
  }
  LOG_IPX("SEND crc:%2x",packetCRC(&outbuffer[0], packetsize));
  if(!isloopback) {
    // Since we're using a channel, we won't send the IP address again
    result = SDLNet_TCP_Send(ipxClientSocket, &outbuffer[0], packetsize);

    if(result == 0) {
      LOG_ERR("IPX: Could not send packet: %s", SDLNet_GetError());
      sendecb->setCompletionFlag(COMP_HARDWAREERROR);
      sendecb->NotifyESR();
      DisconnectFromServer(true);
      return;
    } else {
      sendecb->setCompletionFlag(COMP_SUCCESS);
      LOG_IPX("Packet sent: size: %d",packetsize);
    }
  }
  else sendecb->setCompletionFlag(COMP_SUCCESS);

  if(isloopback||islocalbroadcast) {
    // Send packet back to ourselves.
    receivePacket(&outbuffer[0],packetsize);
    LOG_IPX("Packet back: loopback:%d, broadcast:%d",isloopback,islocalbroadcast);
  }
  sendecb->NotifyESR();
}

static bool pingCheck(IPXHeader * outHeader) {
  IPXHeader *regHeader = readNextIPXHeader(ipxClientSocket);
  if (regHeader != nullptr) {
    assert(SDLNet_Read16(regHeader->length) == sizeof(IPXHeader));
    memcpy(outHeader, regHeader, sizeof(IPXHeader));
    return true;
  }
  return false;
}

bool _ConnectToServer(char const *strAddr) {
  int numsent;
  IPXHeader regHeader;
  if(!SDLNet_ResolveHost(&ipxServConnIp, strAddr, (Bit16u)udpPort)) {

    // Generate the MAC address.  This is made by zeroing out the first two
    // octets and then using the actual IP address for the last 4 octets.
    // This idea is from the IPX over IP implementation as specified in RFC 1234:
    // http://www.faqs.org/rfcs/rfc1234.html

    ipxClientSocket = SDLNet_TCP_Open(&ipxServConnIp);
    if(ipxClientSocket) {
      // Bind UDP port to address to channel
      //UDPChannel = SDLNet_TCP_Bind(ipxClientSocket,-1,&ipxServConnIp);
      //			ipxClientSocket = SDLNet_TCP_Open(&ipxServConnIp);
      SDLNet_Write16(0xffff, regHeader.checkSum);
      SDLNet_Write16(sizeof(regHeader), regHeader.length);

      // Echo packet with zeroed dest and src is a server registration packet
      SDLNet_Write32(0, regHeader.dest.network);
      regHeader.dest.addr.byIP.host = 0x0;
      regHeader.dest.addr.byIP.port = 0x0;
      SDLNet_Write16(0x2, regHeader.dest.socket);

      SDLNet_Write32(0, regHeader.src.network);
      regHeader.src.addr.byIP.host = 0x0;
      regHeader.src.addr.byIP.port = 0x0;
      SDLNet_Write16(0x2, regHeader.src.socket);
      regHeader.transControl = 0;

      // Send registration string to server.  If server doesn't get
      // this, client will not be registered
      numsent = SDLNet_TCP_Send(ipxClientSocket, &regHeader, sizeof(regHeader));

      if(!numsent) {
        LOG_ERR("IPX: Unable to connect to server: %s", SDLNet_GetError());
        SDLNet_TCP_Close(ipxClientSocket);
        return false;
      } else {
        // Wait for return packet from server.
        // This will contain our IPX address and port num
        Bits result;
        Bit32u ticks, elapsed;
        ticks = GetTicks();

        while(true) {
          elapsed = GetTicks() - ticks;
          if(elapsed > 1500) {
            LOG_ERR("Timeout connecting to server at %s", strAddr);
            SDLNet_TCP_Close(ipxClientSocket);

            return false;
          }
          
          asyncify_sleep(4);

          result = SDLNet_TCP_Recv(ipxClientSocket, &regHeader, sizeof(regHeader));
          if (result != 0) {
            memcpy(localIpxAddr.netnode, regHeader.dest.addr.byNode.node, sizeof(localIpxAddr.netnode));
            memcpy(localIpxAddr.netnum, regHeader.dest.network, sizeof(localIpxAddr.netnum));
            break;
          }

        }

        getLogger(LOG_NET, LOG_NORMAL)("IPX: Connected to server.  IPX address is %d:%d:%d:%d:%d:%d, real address %d.%d.%d.%d:%d",
                 CONVIPX(localIpxAddr.netnode), CONVIP(regHeader.dest.addr.byIP.host), SDLNet_Read16(&regHeader.dest.addr.byIP.port));

        incomingPacket.connected = true;
        TIMER_AddTickHandler(&IPX_ClientLoop);
        return true;
      }
    } else {
      LOG_ERR("IPX: Unable to open socket");
    }
  } else {
    LOG_ERR("IPX: Unable resolve connection to server");
  }
  return false;
}

bool ConnectToServer(char const *strAddr) {
  SDLnet_useCallbackIdle = 0;
  bool result = _ConnectToServer(strAddr);
  if (result) {
    client_network_connected(NETWORK_DOSBOX_IPX, strAddr, udpPort);
  } else {
    client_network_disconnected(NETWORK_DOSBOX_IPX);
  }
  SDLnet_useCallbackIdle = 1;
  return result;
}

void IPX_NetworkInit() {

  localIpxAddr.netnum[0] = 0x0;
  localIpxAddr.netnum[1] = 0x0;
  localIpxAddr.netnum[2] = 0x0;
  localIpxAddr.netnum[3] = 0x1;
  localIpxAddr.netnode[0] = 0x00;
  localIpxAddr.netnode[1] = 0x00;
  localIpxAddr.netnode[2] = 0x00;
  localIpxAddr.netnode[3] = 0x00;
  localIpxAddr.netnode[4] = 0x00;
  localIpxAddr.netnode[5] = 0x00;

  socketCount = 0;
  return;
}

class IPXNET : public Program {
 public:
  void HelpCommand(const char *helpStr) {
    // Help on connect command
    if(strcasecmp("connect", helpStr) == 0) {
      WriteOut("IPXNET CONNECT opens a connection to an IPX tunneling server running on another\n");
      WriteOut("DOSBox session.  The \"address\" parameter specifies the IP address or host name\n");
      WriteOut("of the server computer.  One can also specify the UDP port to use.  By default\n");
      WriteOut("IPXNET uses port 213, the assigned IANA port for IPX tunneling, for its\nconnection.\n\n");
      WriteOut("The syntax for IPXNET CONNECT is:\n\n");
      WriteOut("IPXNET CONNECT address <port>\n\n");
      return;
    }
    // Help on the disconnect command
    if(strcasecmp("disconnect", helpStr) == 0) {
      WriteOut("IPXNET DISCONNECT closes the connection to the IPX tunneling server.\n\n");
      WriteOut("The syntax for IPXNET DISCONNECT is:\n\n");
      WriteOut("IPXNET DISCONNECT\n\n");
      return;
    }
    // Help on the startserver command
    if(strcasecmp("startserver", helpStr) == 0) {
      WriteOut("IPXNET STARTSERVER starts and IPX tunneling server on this DOSBox session.  By\n");
      WriteOut("default, the server will accept connections on UDP port 213, though this can be\n");
      WriteOut("changed.  Once the server is started, DOSBox will automatically start a client\n");
      WriteOut("connection to the IPX tunneling server.\n\n");
      WriteOut("The syntax for IPXNET STARTSERVER is:\n\n");
      WriteOut("IPXNET STARTSERVER <port>\n\n");
      return;
    }
    // Help on the stop server command
    if(strcasecmp("stopserver", helpStr) == 0) {
      WriteOut("IPXNET STOPSERVER stops the IPX tunneling server running on this DOSBox\nsession.");
      WriteOut("  Care should be taken to ensure that all other connections have\nterminated ");
      WriteOut("as well since stopping the server may cause lockups on other\nmachines still using ");
      WriteOut("the IPX tunneling server.\n\n");
      WriteOut("The syntax for IPXNET STOPSERVER is:\n\n");
      WriteOut("IPXNET STOPSERVER\n\n");
      return;
    }
    // Help on the ping command
    if(strcasecmp("ping", helpStr) == 0) {
      WriteOut("IPXNET PING broadcasts a ping request through the IPX tunneled network.  In    \n");
      WriteOut("response, all other connected computers will respond to the ping and report\n");
      WriteOut("the time it took to receive and send the ping message.\n\n");
      WriteOut("The syntax for IPXNET PING is:\n\n");
      WriteOut("IPXNET PING\n\n");
      return;
    }
    // Help on the status command
    if(strcasecmp("status", helpStr) == 0) {
      WriteOut("IPXNET STATUS reports the current state of this DOSBox's sessions IPX tunneling\n");
      WriteOut("network.  For a list of the computers connected to the network use the IPXNET \n");
      WriteOut("PING command.\n\n");
      WriteOut("The syntax for IPXNET STATUS is:\n\n");
      WriteOut("IPXNET STATUS\n\n");
      return;
    }
  }

  void Run(void)
  {
    WriteOut("IPX Tunneling utility for DOSBox\n\n");
    if(!cmd->GetCount()) {
      WriteOut("The syntax of this command is:\n\n");
      WriteOut("IPXNET [ CONNECT | DISCONNECT | STARTSERVER | STOPSERVER | PING | HELP |\n         STATUS ]\n\n");
      return;
    }

    if(cmd->FindCommand(1, temp_line)) {
      if(strcasecmp("help", temp_line.c_str()) == 0) {
        if(!cmd->FindCommand(2, temp_line)) {
          WriteOut("The following are valid IPXNET commands:\n\n");
          WriteOut("IPXNET CONNECT        IPXNET DISCONNECT       IPXNET STARTSERVER\n");
          WriteOut("IPXNET STOPSERVER     IPXNET PING             IPXNET STATUS\n\n");
          WriteOut("To get help on a specific command, type:\n\n");
          WriteOut("IPXNET HELP command\n\n");

        } else {
          HelpCommand(temp_line.c_str());
          return;
        }
        return;
      }
      if(strcasecmp("connect", temp_line.c_str()) == 0) {
        char strHost[1024];
        if(incomingPacket.connected) {
          WriteOut("IPX Tunneling Client already connected.\n");
          return;
        }
        if(!cmd->FindCommand(2, temp_line)) {
          WriteOut("IPX Server address not specified.\n");
          return;
        }
        strcpy(strHost, temp_line.c_str());

        if(!cmd->FindCommand(3, temp_line)) {
          udpPort = 213;
        } else {
          udpPort = strtol(temp_line.c_str(), NULL, 10);
        }

        if(ConnectToServer(strHost)) {
          WriteOut("IPX Tunneling Client connected to server at %s.\n", strHost);
        } else {
          WriteOut("IPX Tunneling Client failed to connect to server at %s.\n", strHost);
        }
        return;
      }

      if(strcasecmp("disconnect", temp_line.c_str()) == 0) {
        if(!incomingPacket.connected) {
          WriteOut("IPX Tunneling Client not connected.\n");
          return;
        }
        // TODO: Send a packet to the server notifying of disconnect
        WriteOut("IPX Tunneling Client disconnected from server.\n");
        DisconnectFromServer(false);
        return;
      }

      if(strcasecmp("status", temp_line.c_str()) == 0) {
        WriteOut("IPX Tunneling Status:\n\n");
        WriteOut("Server status: ");
        WriteOut("INACTIVE\n");
        WriteOut("Client status: ");
        if(incomingPacket.connected) {
          WriteOut("CONNECTED -- Server at %d.%d.%d.%d port %d\n", CONVIP(ipxServConnIp.host), udpPort);
        } else {
          WriteOut("DISCONNECTED\n");
        }
        return;
      }

      if(strcasecmp("ping", temp_line.c_str()) == 0) {
        constexpr int pingTimeoutMs = 1500;
        bool wasPing = false;
        int iteration = 0;
        Bit32u startTicks;
        Bit32u ticks;
        IPXHeader pingHead;

        if(!incomingPacket.connected) {
          WriteOut("IPX Tunneling Client not connected.\n");
          return;
        }
        TIMER_DelTickHandler(&IPX_ClientLoop);
        WriteOut("Sending broadcast ping:\n\n");
        startTicks = GetTicks();

        do {
          iteration++;
          pingSend();

          do {
            CALLBACK_Idle();
            ticks = GetTicks();
            wasPing = pingCheck(&pingHead);
          } while ((ticks - startTicks) < pingTimeoutMs && !wasPing);

          if (wasPing) {
            WriteOut("%d: %d.%d.%d.%d, port %d time=%dms\n", iteration,
                     CONVIP(pingHead.src.addr.byIP.host),
                     SDLNet_Read16(&pingHead.src.addr.byIP.port),
                     GetTicks() - ticks);
            getLogger(LOG_NET, LOG_NORMAL)("%d: %d.%d.%d.%d, port %d time=%dms", iteration,
                                           CONVIP(pingHead.src.addr.byIP.host),
                                           SDLNet_Read16(&pingHead.src.addr.byIP.port),
                                           GetTicks() - ticks);
          }
        } while((ticks - startTicks) < pingTimeoutMs);
        TIMER_AddTickHandler(&IPX_ClientLoop);
        return;
      }
    }
  }
};

static void IPXNET_ProgramStart(Program * * make) {
  *make=new IPXNET;
}

Bitu IPX_ESRHandler(void) {
  LOG_IPX("ESR: >>>>>>>>>>>>>>>" );
  while(ESRList!=NULL) {
    // LOG_IPX("ECB: SN%7d notified.", ESRList->SerialNumber);
    if(ESRList->databuffer) ESRList->writeData();
    if(ESRList->getESRAddr()) {
      // setup registers
      SegSet16(es, RealSeg(ESRList->ECBAddr));
      reg_si = RealOff(ESRList->ECBAddr);
      reg_al = 0xff;
      CALLBACK_RunRealFar(RealSeg(ESRList->getESRAddr()),
                          RealOff(ESRList->getESRAddr()));
    }
    delete ESRList; //Destructor updates this pointer to the next value or NULL
  }	// while

  IO_WriteB(0xa0,0x63);	//EOI11
  IO_WriteB(0x20,0x62);	//EOI2
  LOG_IPX("ESR: <<<<<<<<<<<<<<<");
  return CBRET_NONE;
}

void VFILE_Remove(const char *name);

class IPX: public Module_base {
 private:
  CALLBACK_HandlerObject callback_ipx;
  CALLBACK_HandlerObject callback_esr;
  CALLBACK_HandlerObject callback_ipxint;
  RealPt old_73_vector;
  static Bit16u dospage;
 public:
  IPX(Section* configuration):Module_base(configuration) {
    Section_prop * section = static_cast<Section_prop *>(configuration);
    if(!section->Get_bool("ipx")) return;
    if(!SDLNetInited) {
      if(SDLNet_Init() == -1){
        LOG_ERR("SDLNet_Init failed: %s\n", SDLNet_GetError());
        return;
      }
      SDLNetInited = true;
    }

    ECBList = NULL;
    ESRList = NULL;
    IPX_NetworkInit();

    DOS_AddMultiplexHandler(IPX_Multiplex);

    callback_ipx.Install(&IPX_Handler,CB_RETF,"IPX Handler");
    ipx_callback = callback_ipx.Get_RealPointer();

    callback_ipxint.Install(&IPX_IntHandler,CB_IRET,"IPX (int 7a)");
    callback_ipxint.Set_RealVec(0x7a);

    callback_esr.Allocate(&IPX_ESRHandler,"IPX_ESR");
    Bit16u call_ipxesr1 = callback_esr.Get_callback();

    if(!dospage) dospage = DOS_GetMemory(2); // can not be freed yet

    PhysPt phyDospage = PhysMake(dospage,0);

    LOG_IPX("ESR callback address: %x, HandlerID %d", phyDospage,call_ipxesr1);

    //save registers
    phys_writeb(phyDospage+0,(Bit8u)0xFA);    // CLI
    phys_writeb(phyDospage+1,(Bit8u)0x60);    // PUSHA
    phys_writeb(phyDospage+2,(Bit8u)0x1E);    // PUSH DS
    phys_writeb(phyDospage+3,(Bit8u)0x06);    // PUSH ES
    phys_writew(phyDospage+4,(Bit16u)0xA00F); // PUSH FS
    phys_writew(phyDospage+6,(Bit16u)0xA80F); // PUSH GS

    // callback
    phys_writeb(phyDospage+8,(Bit8u)0xFE);  // GRP 4
    phys_writeb(phyDospage+9,(Bit8u)0x38);  // Extra Callback instruction
    phys_writew(phyDospage+10,call_ipxesr1);        // Callback identifier

    // register recreation
    phys_writew(phyDospage+12,(Bit16u)0xA90F); // POP GS
    phys_writew(phyDospage+14,(Bit16u)0xA10F); // POP FS
    phys_writeb(phyDospage+16,(Bit8u)0x07);    // POP ES
    phys_writeb(phyDospage+17,(Bit8u)0x1F);    // POP DS
    phys_writeb(phyDospage+18,(Bit8u)0x61);    // POPA
    phys_writeb(phyDospage+19,(Bit8u)0xCF);    // IRET: restores flags, CS, IP

    // IPX version 2.12
    //phys_writeb(phyDospage+27,(Bit8u)0x2);
    //phys_writeb(phyDospage+28,(Bit8u)0x12);
    //IPXVERpointer = RealMake(dospage,27);

    RealPt ESRRoutineBase = RealMake(dospage, 0);

    // Interrupt enabling
    RealSetVec(0x73,ESRRoutineBase,old_73_vector);	// IRQ11
    IO_WriteB(0xa1,IO_ReadB(0xa1)&(~8));			// enable IRQ11

    PROGRAMS_MakeFile("IPXNET.COM",IPXNET_ProgramStart);
  }

  ~IPX() {
    Section_prop * section = static_cast<Section_prop *>(m_configuration);
    PIC_RemoveEvents(IPX_AES_EventHandler);
    if(!section->Get_bool("ipx")) return;

    DisconnectFromServer(false);

    DOS_DelMultiplexHandler(IPX_Multiplex);
    RealSetVec(0x73,old_73_vector);
    IO_WriteB(0xa1,IO_ReadB(0xa1)|8);	// disable IRQ11

    PhysPt phyDospage = PhysMake(dospage,0);
    for(Bitu i = 0;i < 32;i++)
      phys_writeb(phyDospage+i,(Bit8u)0x00);

    VFILE_Remove("IPXNET.COM");
  }
};

static IPX* test;

void IPX_ShutDown(Section* sec) {
  delete test;
}

void IPX_Init(Section* sec) {
  test = new IPX(sec);
  sec->AddDestroyFunction(&IPX_ShutDown,true);
}

//Initialize static members;
Bit16u IPX::dospage = 0;

void server_network_connect(NetworkType networkType, const char* address, uint32_t port) {
  if (incomingPacket.connected) {
    client_network_disconnected(NETWORK_DOSBOX_IPX);
    return;
  }

  udpPort = port;
  ConnectToServer(address);
}

void server_network_disconnect(NetworkType networkType) {
  if (!incomingPacket.connected) {
    client_network_disconnected(networkType);
    return;
  }

  DisconnectFromServer(false);
}

#endif
