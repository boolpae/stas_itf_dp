
#include "ItfDpWorker.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <thread>
#include <chrono>

#define INIT_BUFF_SIZE 2048

ITF_DP_Worker::ITF_DP_Worker(string ipaddr, int port)
: m_sBrokerIp(ipaddr), m_nBrokerPort(port)
{
    m_pTransBuff = new char[ INIT_BUFF_SIZE ];
    m_nTransBuffSize = INIT_BUFF_SIZE;

}

ITF_DP_Worker::~ITF_DP_Worker()
{
    delete (m_pTransBuff);
    close(m_sockfd);

}

// 설정된 IP, Port로 TCP 접속을 시도 후 연결이 성공되면 0, 실패한 경우 0이 아닌 값 리턴
int ITF_DP_Worker::init(string type)
{
    int ret=0;
	struct sockaddr_in addr;
    char buffer[128] = {0};
    int plen, nRecvSize;

    m_sType = type;

	if ((m_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("ITF_DP_Worker::init() :");
        // m_Logger->error("VDClient::init() - failed get socket : %d", errno);
		m_sockfd = 0;
		return -1;	// socket 생성 오류
	}

	memset(&addr, 0x00, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(m_sBrokerIp.c_str());
	addr.sin_port = htons(m_nBrokerPort);

    if (connect(m_sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) 
    { 
        perror("ITF_DP_Worker::init() : Connection Failed -"); 
        return -1; 
    }

    send( m_sockfd, m_sType.c_str(), m_sType.length(), 0) ;

    recv( m_sockfd, &plen, sizeof(plen), 0 );
    nRecvSize = ntohl(plen);

    memset(buffer, 0, sizeof(buffer));
    plen = 0;
    while ( (plen += recv( m_sockfd , buffer+plen, (nRecvSize - plen), 0)) != nRecvSize );

    if ( !strncmp(buffer, "VR_", 3) )
    {
        m_sWorkerID = buffer;
        return 0;
    }

    printf("Connection Failed (Invalid Type - %s)\n", m_sType.c_str()); 
    return -2;
}

int ITF_DP_Worker::waitJob()
{
    int nTransSize=0;
    int nRecvLen=0;
    uint32_t plen=0;

    // 1. recv total packet length value(uint32_t, ntohl)
    while ( (nTransSize = recv(m_sockfd, &plen, sizeof(uint32_t), 0)) <= 0 )
    {
        close(m_sockfd);
        while ( init(m_sType) )
        {
            std::this_thread::sleep_for( std::chrono::seconds(1) ) ;
        }
    }

    // 2. recv total packet
    nRecvLen = ntohl(plen);

    if ( nRecvLen > m_nTransBuffSize ) {
        m_nTransBuffSize = nRecvLen + 1;
        delete m_pTransBuff;
        m_pTransBuff = new char[ m_nTransBuffSize ];
    }
    
    nTransSize = 0;
    memset(m_pTransBuff, 0, m_nTransBuffSize);
    while ( (nTransSize += recv(m_sockfd, (void *)(m_pTransBuff + nTransSize), (nRecvLen - nTransSize), 0)) != nRecvLen );

    m_nValueSize = nTransSize;
    return m_nValueSize;

}

int ITF_DP_Worker::responseJob(void *data, int len)
{
    int nTransSize=0;
    int nRecvLen=0;
    uint32_t plen=0;
    char *pData = (char *)data;


    plen = htonl(len);
    // 1. send total packet length value(uint32_t, htonl)
    memcpy(m_pTransBuff, &plen, sizeof(uint32_t));
    nTransSize = send(m_sockfd, (const void*)m_pTransBuff, sizeof(uint32_t), 0);

    // 2. send total packet
    nTransSize =0;
    while ( (nTransSize += send(m_sockfd, (const void*)(pData + nTransSize), (len - nTransSize), 0)) < len );

    return nTransSize;
}
