#ifdef  ENABLE_REALTIME

#include "VDClient.h"

#ifdef USE_REALTIME_MT
#include "VRClientMT.h"
#else
#include "VRClient.h"
#endif

#include "VRCManager.h"
#include "WorkTracer.h"
#include "HAManager.h"
#include "stas.h"

#include <thread>
#include <iostream>
#include <fstream>

#ifndef WIN32
#include <unistd.h>
#include <netinet/in.h>
#include <string.h>

#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>

#include <arpa/inet.h>

#undef htons
#undef ntohs

#else

#include <WS2tcpip.h> // for socklen_t type

#endif

VDClient::VDClient(VRCManager *vrcm/*, log4cpp::Category *logger*/)
	: m_nLiveFlag(1), m_nWorkStat(0), m_nPort(0), m_nSockfd(0), m_sCallId(""), m_nSpkNo(0), m_vrcm(vrcm), /*m_Logger(logger),*/ m_nPlaytime(3*16000)
{
	m_pVrc = NULL;
	m_tTimeout = time(NULL);

	//printf("\t[DEBUG] VDClinet Constructed.\n");
	m_Logger = config->getLogger();
    m_Logger->debug("VDClinet Constructed.");
}

void VDClient::finish()
{
	m_nLiveFlag = 0;

	// 인스턴스 생성 후 init() 실패 후 finish() 호출한 경우, 생성된 인스턴스 삭제
	if (m_nSockfd) {
        close(m_nSockfd);
		m_thrd.detach();//
        //m_thrd.join();
	}

	delete this;

}

uint16_t VDClient::init(uint16_t port)
{
	struct sockaddr_in addr;

	if ((m_nSockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		perror("VDClient::init() :");
        m_Logger->error("VDClient::init() - failed get socket : %d", errno);
		m_nSockfd = 0;
		return uint16_t(1);	// socket 생성 오류
	}

	memset(&addr, 0x00, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (::bind(m_nSockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("VDClient::init() - bind : ");
        m_Logger->error("VDClient::init() - failed get bind : %d", errno);
		closesocket(m_nSockfd);
		m_nSockfd = 0;
		return uint16_t(2);	// bind 오류
	}

	m_nPort = port;

	//printf("\t[DEBUG] VDClient::init() - port(%d)\n", port);
    m_Logger->info("VDClient::init() - port(%d)", port);

	m_thrd = std::thread(VDClient::thrdMain, this);

	return uint16_t(0);
}

#define BUFLEN 65000  //Max length of buffer
#define VOICE_BUFF_LEN (16000 * 5 + 64)
#define LEN_OF_VOICE ( 16000 * 5 )
void VDClient::thrdMain(VDClient * client)
{
	char buf[BUFLEN];
	struct sockaddr_in si_other;
	struct timeval tv;
	fd_set rfds;
	int selVal;
	int recv_len;
	socklen_t slen = sizeof(si_other);
	QueItem* item = NULL;
	char* pos = NULL;
	uint16_t nVDSize;
#if 0 // save pcm file
    int nRecvCount=0;
    std::string filename;
    std::ofstream pcmFile;
#endif
	while (client->m_nLiveFlag) {
		//clear the buffer by filling null, it might have previously received data
		tv.tv_sec = 0;	// for debug
		tv.tv_usec = 500000;
		FD_ZERO(&rfds);
		FD_SET(client->m_nSockfd, &rfds);

		selVal = select(client->m_nSockfd+1, &rfds, NULL, NULL, &tv);

		if (selVal > 0) {
			memset(buf, '\0', BUFLEN);

			//try to receive some data, this is a blocking call
			if ((recv_len = recvfrom(client->m_nSockfd, buf, BUFLEN - 1, 0, (struct sockaddr *) &si_other, &slen)) == -1)
			{
				perror("VDClient::thrdMain() - recvfrom() failed with error :");
                client->m_Logger->error("VDClient::thrdMain() - recvfrom() failed with error : %d", errno);
				break;
			}

			pos = buf;
			// 인입된 UDP패킷이 RT-STT 음성 패킷이 아닌 경우 처리하지 않음
			if (memcmp(pos, "RT-STT", 6)) {
				//printf("\t[DEBUG] Invalid Voice Data Packet - VDClient(%d) recv_len(%d), pVrc(0x%p), nWorkStat(%d)\n", client->m_nPort, recv_len, client->m_pVrc, client->m_nWorkStat);
                client->m_Logger->warn("VDClient::thrdMain() - Invalid Voice Data Packet - VDClient(%d) recv_len(%d), pVrc(0x%p), nWorkStat(%d)", client->m_nPort, recv_len, client->m_pVrc, client->m_nWorkStat);
				continue;
			}
			pos += 6;
			memcpy(&nVDSize, pos, sizeof(uint16_t));
			recv_len = ::ntohs(nVDSize);
			pos += sizeof(uint16_t);

			// printf("\t[DEBUG] VDClient(%d) recv_len(%d), pVrc(%X), nWorkStat(%d), network_len(%d)\n", client->m_nPort, recv_len, client->m_pVrc, client->m_nWorkStat, nVDSize);
			// m_nWorkStat 상태가 대기가 아닐 경우에만 recvfrom한 buf 내용으로 작업을 진행한다.
			// m_nWorkStat 상태가 대기(0)인 경우 recvfrom() 수행하여 수집된 데이터는 버림
			if (recv_len && client->m_pVrc && client->m_nWorkStat) {
				client->m_tTimeout = time(NULL);
				if (!item) {
					item = new QueItem;
					item->voiceData = new uint8_t[VOICE_BUFF_LEN];
                    // 시작 패킷 표시
                    if (client->m_nWorkStat == 3) {
                        item->flag = 2;
                        client->m_nWorkStat = 1;
                    }
                    else {
                        item->flag = 1;
                    }
					item->spkNo = client->m_nSpkNo;
					item->lenVoiceData = 0;
					memset(item->voiceData, 0x00, VOICE_BUFF_LEN);
				}
#if 0 // save pcm file
                nRecvCount++;
                filename = client->getCallId() + std::string("_") + std::to_string(client->getSpkNo()) + std::string(".pcm");
                pcmFile.open(filename, ios::out | ios::app | ios::binary);
				if (pcmFile.is_open()) {
					pcmFile.write((const char*)pos, recv_len);
                    pcmFile.close();
				}
#endif
				memcpy(item->voiceData + item->lenVoiceData, pos, recv_len);
				item->lenVoiceData += recv_len;
                
                // printf("\t[DEBUG] VDClient::thrdMain() - VoiceDataLen(%d)\n", item->lenVoiceData);

				if (item->lenVoiceData >= client->m_nPlaytime) {
                    // printf("\t[DEBUG] ItemVoiceSize(%d)\n", item->lenVoiceData);
					client->m_pVrc->insertQueItem(item);
					item = NULL;
				}

			}

			if (client->m_pVrc && (client->m_nWorkStat == 2)) {	// 호 종료 요청이 들어왔을 때
			END_CALL:
                if (!HAManager::getInstance() || HAManager::getInstance()->getHAStat()) {
                    if (client->m_pVrc) {
                        //printf("\t[DEBUG] VDClient(%d) work ending...(%d)\n", client->m_nPort, nRecvCount);
                        client->m_Logger->debug("VDClient::thrdMain() - VDClient(%d) work ending...", client->m_nPort);
                        if (!item) {
                            item = new QueItem;
                            item->voiceData = NULL;
                            item->lenVoiceData = 0;
                            item->spkNo = client->m_nSpkNo;
                        }

                        item->flag = 0;
                        if (recv_len) {
                            if (!item->voiceData) {
                                item->voiceData = new uint8_t[VOICE_BUFF_LEN];
                                memset(item->voiceData, 0x00, VOICE_BUFF_LEN);
                            }
                            memcpy(item->voiceData + item->lenVoiceData, pos, recv_len);
                            item->lenVoiceData += recv_len;
                        }

                        client->m_pVrc->insertQueItem(item);
                        item = NULL;
                    }

                }
				// 작업 종료 요청 후 마지막 데이터 처리 후 상태를 대기 상태로 전환
				client->m_sCallId = "";
				client->m_nSpkNo = 0;
				client->m_nWorkStat = 0;
				client->m_pVrc = NULL;
#if 0 // save pcm file
                nRecvCount=0;
#endif
			}
		}
		else if ((selVal == 0) && ((client->m_nWorkStat == 3) || (client->m_nWorkStat == 1))) {	// 이 로직은 수정해야할 필요가 있다. 현재는 30초동안 데이터가 안들어 올 경우 호를 종료
            if (HAManager::getInstance() && !HAManager::getInstance()->getHAStat()) {
                continue;
            }
            
			// timeout : 현재 30초로 고정
			if ((time(NULL) - client->m_tTimeout) > 30) {
				WorkTracer::instance()->insertWork(client->m_sCallId, 'R', WorkQueItem::PROCTYPE::R_END_VOICE, client->m_nSpkNo);

                client->m_Logger->debug("VDClient::thrdMain(%d) - Working... timeout(%llu)", client->m_nPort, (time(NULL) - client->m_tTimeout));
				recv_len = 0;
				goto END_CALL;
			}

			//printf("\t[DEBUG] VDClient::thrdMain(%d) - Working... timeout(%llu)\n", client->m_nPort, (time(NULL) - client->m_tTimeout));
            //client->m_Logger->debug("VDClient::thrdMain(%d) - Working... timeout(%llu)", client->m_nPort, (time(NULL) - client->m_tTimeout));
		}
		else if ((selVal == 0) && (client->m_nWorkStat == 2)) {
			recv_len = 0;
			goto END_CALL;
		}

		// timeout에 의한 호 종료 처리 로직 필요
		//printf("\t[DEBUG] VDClient(%d) thread running...\n", client->m_nPort);
	}
}

VDClient::~VDClient()
{
	if (m_nSockfd) closesocket(m_nSockfd);

	//printf("\t[DEBUG] VDClinet Destructed.(%d)\n", m_nPort);
    //m_Logger->debug("VDClinet Destructed.(%d)", m_nPort);
}

void VDClient::startWork(std::string& callid, uint8_t spkno)
{
	m_sCallId = callid;
	m_nSpkNo = spkno;
	m_tTimeout = time(NULL);
	m_nWorkStat = uint8_t(3);

    m_pVrc = m_vrcm->getVRClient(callid);

	WorkTracer::instance()->insertWork(m_sCallId, 'R', WorkQueItem::PROCTYPE::R_BEGIN_VOICE, m_nSpkNo);
}

#endif // ENABLE_REALTIME