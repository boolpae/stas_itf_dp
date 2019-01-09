
#include "ItfDpBroker.h"

#include <unistd.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include <iostream>

#include <chrono>

ITF_DP_Broker* ITF_DP_Broker::m_instance = nullptr;

int lBrokerExit=0;


// ITF_DP_Client Section /////////////////////////////////////////////////////////////////////////
#define INIT_BUFF_SIZE 2048

ITF_DP_Client::ITF_DP_Client(ITF_DP_Broker *brk, string fname, SOCKET sockfd)
: m_brk(brk), m_fname(fname), m_state(0), m_sockfd(sockfd)
{
    m_pTransBuff = new char[ INIT_BUFF_SIZE ];
    m_nTransBuffSize = INIT_BUFF_SIZE;
    m_nValueSize = 0;

    thread thrdKeepAlive = thread(ITF_DP_Client::thrdKeepAlive, this);
    thrdKeepAlive.detach();
    cout << "ITF_DP_Client("<<m_fname<<") created." << endl;
}

ITF_DP_Client::~ITF_DP_Client()
{
    cout << "ITF_DP_Client("<<m_fname<<") destroyed." << endl;
    delete (m_pTransBuff);
    close(m_sockfd);
}

void ITF_DP_Client::thrdKeepAlive(ITF_DP_Client *clt)
{
    int retval;
    char buf[8];

    while(!lBrokerExit) {
        if ( !clt->m_state ) {
            retval = recv(clt->m_sockfd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
            cout << "WORKER(" << clt->m_fname << ") SOCKET STATE[" << retval<< "]" << endl;

            if ( !retval ) // disconnected
            {
                clt->m_brk->eraseWorker(clt->m_fname);
                break;
            }
        }
        std::this_thread::sleep_for( std::chrono::seconds(1));
    }
}

// DESC
// worker 즉, TCP로 연결된 client로 작업 요청 데이터를 보낸 후 응답을 받아 전달하는 역할을 담당한다.
// RETURN : void *m_pValue - 응답 데이터
// void *data : 전송할 데이터
// int len : 전송할 데이터의 길이
int ITF_DP_Client::requestJob(void *data, int len)
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

    // 3. recv total packet length value(uint32_t, ntohl)
    nTransSize = recv(m_sockfd, &plen, sizeof(uint32_t), 0);

    // 4. recv total packet
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


// ITF_DP_Broker Section /////////////////////////////////////////////////////////////////////////

ITF_DP_Broker::ITF_DP_Broker(int sock)
: m_nMainSocket(sock)
{
	pthread_mutex_init( &m_brkMutex, NULL );
}

ITF_DP_Broker::~ITF_DP_Broker()
{
    close(m_nMainSocket);

    if ( m_mWorkerTable.size() )
    {
        map< string, ITF_DP_Client* >::iterator iter;

        for(iter = m_mWorkerTable.begin(); iter != m_mWorkerTable.end(); iter++)
        {
            delete iter->second;
        }

        m_mWorkerTable.clear();
    }

    pthread_mutex_destroy( &m_brkMutex );

}

ITF_DP_Broker* ITF_DP_Broker::instance(int port)
{
    if (m_instance) return m_instance;

	struct sockaddr_in addr;
    SOCKET nSockfd=0;
    thread thrdListner;
    int sockOptVal=1;

	if ((nSockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("ITF_DP_Broker::instance() :");
        // m_Logger->error("VDClient::init() - failed get socket : %d", errno);
		nSockfd = 0;
		return nullptr;	// socket 생성 오류
	}

    if ( setsockopt(nSockfd, SOL_SOCKET, SO_REUSEADDR, &sockOptVal, sizeof(int)) < 0 )
    {
        close(nSockfd);
        return nullptr;
    }

	memset(&addr, 0x00, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (::bind(nSockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("ITF_DP_Broker::instance() - bind : ");
        // m_Logger->error("VDClient::init() - failed get bind : %d", errno);
		close(nSockfd);
		nSockfd = 0;
		return nullptr;	// bind 오류
	}

    if(listen(nSockfd, 200) < 0)
    {//소켓을 수동 대기모드로 설정
        perror("ITF_DP_Broker::instance() - Can't listening connect.\n");
		close(nSockfd);
		nSockfd = 0;
		return nullptr;	// listen 오류
    }

    m_instance = new ITF_DP_Broker(nSockfd);

    lBrokerExit = 0;

    thrdListner = thread(ITF_DP_Broker::thrdListner, m_instance);
    thrdListner.detach();

#if 0
    thrdListner = thread(ITF_DP_Broker::thrdCheckClientSocket, m_instance);
    thrdListner.detach();
#endif

    return m_instance;
}

void ITF_DP_Broker::release()
{
    lBrokerExit = 1;
    if (m_instance) delete m_instance;
}

#define BUF_LEN 128
void ITF_DP_Broker::thrdListner(ITF_DP_Broker *brk)
{
    char buffer[BUF_LEN];
    struct sockaddr_in client_addr;
    char temp[20];
    int client_fd;
    // client_fd : 각 소켓 번호
    int len, msg_size;
    ITF_DP_Client *clt;
    struct timeval tVal;
    struct tm* timeinfo;
    char fname[128];
    uint32_t plen;

    memset(buffer, 0x00, sizeof(buffer));
    len = sizeof(client_addr);

    while(!lBrokerExit)
    {
        client_fd = accept(brk->m_nMainSocket, (struct sockaddr *)&client_addr, (socklen_t*)&len);
        if(client_fd < 0)
        {
            printf("Server: accept failed.\n");
            continue;
        }
        inet_ntop(AF_INET, &client_addr.sin_addr.s_addr, temp, sizeof(temp));
        printf("Server : %s client connected.\n", temp);
    
        msg_size = read(client_fd, buffer, BUF_LEN);
        buffer[msg_size] = 0;

        // REALTIME
        if ( !memcmp("REALTIME", buffer, 8) ) {
            memset(fname, 0, sizeof(fname));
            gettimeofday( &tVal, NULL );
            timeinfo = localtime( &(tVal.tv_sec) );
            strftime(fname, sizeof(fname), "VR_REALTIME_%Y%m%d%H%M%S_", timeinfo);
            sprintf(fname+strlen(fname), "%ld", tVal.tv_usec);

        }
        // STT
        else if ( !memcmp("STT_NT", buffer, 6) ) {
            memset(fname, 0, sizeof(fname));
            gettimeofday( &tVal, NULL );
            timeinfo = localtime( &(tVal.tv_sec) );
            strftime(fname, sizeof(fname), "VR_STT_NT_%Y%m%d%H%M%S_", timeinfo);
            sprintf(fname+strlen(fname), "%ld", tVal.tv_usec);

        }
        // STT with time
        else if ( !memcmp("STT_WT", buffer, 6) ) {
            memset(fname, 0, sizeof(fname));
            gettimeofday( &tVal, NULL );
            timeinfo = localtime( &(tVal.tv_sec) );
            strftime(fname, sizeof(fname), "VR_STT_WT_%Y%m%d%H%M%S_", timeinfo);
            sprintf(fname+strlen(fname), "%ld", tVal.tv_usec);

        }
        // UNSEGMENT
        else if ( !memcmp("UNSEG", buffer, 5) ) {
            memset(fname, 0, sizeof(fname));
            gettimeofday( &tVal, NULL );
            timeinfo = localtime( &(tVal.tv_sec) );
            strftime(fname, sizeof(fname), "VR_UNSEG_%Y%m%d%H%M%S_", timeinfo);
            sprintf(fname+strlen(fname), "%ld", tVal.tv_usec);

        }
        else {
            sprintf(buffer, "NOT ACCEPTED");
            len = strlen(buffer);
            plen = htonl(len);
            // 1. send total packet length value(uint32_t, htonl)
            send(client_fd, (const void*)&plen, sizeof(uint32_t), 0);
            msg_size = strlen(buffer);
            write(client_fd, buffer, msg_size);
            close(client_fd);
            client_fd = -1;
            printf("Server : %s client closed.(InvalidType: %s)\n", temp, buffer);
        }

        if ( client_fd > 0 ) {
            clt = new ITF_DP_Client(brk, string(fname), client_fd);
            if ( clt )
            {
                brk->m_mWorkerTable.insert(make_pair(fname, clt));

                len = strlen(fname);
                plen = htonl(len);
                // 1. send total packet length value(uint32_t, htonl)
                send(client_fd, (const void*)&plen, sizeof(uint32_t), 0);

                // 2. send total packet
                msg_size =0;
                while ( (msg_size += send(client_fd, (const void*)(fname + msg_size), (len - msg_size), 0)) < len );
            }
            else
            {
                sprintf(buffer, "FAILED CONNECTED");
                len = strlen(buffer);
                plen = htonl(len);
                // 1. send total packet length value(uint32_t, htonl)
                send(client_fd, (const void*)&plen, sizeof(uint32_t), 0);
                msg_size = strlen(buffer);
                write(client_fd, buffer, msg_size);
                close(client_fd);
                client_fd = -1;
                printf("Server : %s client closed.(InvalidType: %s)\n", temp, buffer);
            }

            // sprintf(buffer, "CONNECTED");
            // msg_size = strlen(buffer);
            // write(client_fd, buffer, msg_size);
        }

    }
    close(brk->m_nMainSocket);

}


#if 0 // DISABEL
void ITF_DP_Broker::thrdCheckClientSocket(ITF_DP_Broker *brk)
{
    // int error = 0;
    // socklen_t len = sizeof (error);
    int retval;
    char buf[8];
    ITF_DP_Client clt;

    while(!lBrokerExit) {
        if ( brk->m_mRTTable.size() )
        {
            map< string, ITF_DP_Client >::iterator iter;

            for(iter = brk->m_mRTTable.begin(); iter != brk->m_mRTTable.end(); iter++)
            {
                // retval = getsockopt (iter->second, SOL_SOCKET, SO_ERROR, &error, &len);
                clt = iter->second;
                if (clt.state) continue;
                retval = recv(clt.sockfd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
                cout << "RT-TABLE SOCKET STATE[" << iter->first <<" : "<<retval<<"]" << endl;

                if ( !retval ) // disconnected
                {
                    close(clt.sockfd);
                    brk->m_mRTTable.erase(iter->first);
                }
            }
        }

        if ( brk->m_mSTTTable.size() )
        {
            map< string, ITF_DP_Client >::iterator iter;

            for(iter = brk->m_mSTTTable.begin(); iter != brk->m_mSTTTable.end(); iter++)
            {
                // retval = getsockopt (iter->second, SOL_SOCKET, SO_ERROR, &error, &len);
                clt = iter->second;
                if (clt.state) continue;
                retval = recv(clt.sockfd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
                cout << "STT-TABLE SOCKET STATE[" << iter->first <<" : "<<retval<<"]" << endl;

                if ( !retval ) // disconnected
                {
                    close(clt.sockfd);
                    brk->m_mSTTTable.erase(iter->first);
                }
            }
        }

        if ( brk->m_mUNSEGTable.size() )
        {
            map< string, ITF_DP_Client >::iterator iter;

            for(iter = brk->m_mUNSEGTable.begin(); iter != brk->m_mUNSEGTable.end(); iter++)
            {
                // retval = getsockopt (iter->second, SOL_SOCKET, SO_ERROR, &error, &len);
                clt = iter->second;
                if (clt.state) continue;
                retval = recv(clt.sockfd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
                cout << "UNSEG-TABLE SOCKET STATE[" << iter->first <<" : "<<retval<<"]" << endl;

                if ( !retval ) // disconnected
                {
                    close(clt.sockfd);
                    brk->m_mUNSEGTable.erase(iter->first);
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::/*milli*/seconds(1));
    }
}
#endif

void ITF_DP_Broker::outputTableState()
{
    cout << "ITF_DP_Client Table size    : [" << m_mWorkerTable.size() << "]" << endl;
    // cout << "STT Table size   : [" << m_mSTTTable.size() << "]" << endl;
    // cout << "UNSEG Table size : [" << m_mUNSEGTable.size() << "]" << endl;
}

void ITF_DP_Broker::eraseWorker(string fname)
{
    map< string, ITF_DP_Client* >::iterator iter;

    iter = m_mWorkerTable.find(fname);
    if (iter != m_mWorkerTable.end()) {
        delete iter->second;
        m_mWorkerTable.erase(iter);
    }

}

ITF_DP_Client* ITF_DP_Broker::requestWorker(string fname)
{
    map< string, ITF_DP_Client* >::iterator iter;

    iter = m_mWorkerTable.find(fname);
    if (iter != m_mWorkerTable.end() ) return iter->second;

    return nullptr;
}

void ITF_DP_Broker::restoreWorker(ITF_DP_Client *clt)
{
    // mutex lock and worker state change 1 > 0
    pthread_mutex_lock( &m_brkMutex );

    clt->m_state = 0;

    pthread_mutex_unlock( &m_brkMutex );

}

void ITF_DP_Broker::restoreWorker(string fname)
{
    // mutex lock and worker state change 1 > 0
    map< string, ITF_DP_Client* >::iterator iter;

    iter = m_mWorkerTable.find(fname);

    if ( iter != m_mWorkerTable.end() ) {
        ITF_DP_Client *clt = iter->second;
        pthread_mutex_lock( &m_brkMutex );

        clt->m_state = 0;

        pthread_mutex_unlock( &m_brkMutex );
    }
}

const char *WORKER_TYPE[] = {"REALTIME","STT_NT","STT_WT","UNSEG"};

string ITF_DP_Broker::reserveWorker(int type)
{
    // mutex lock and worker state change 0 > 1
    string fname="";
    map< string, ITF_DP_Client* >::iterator iter;

    if (type>3) return fname;

    pthread_mutex_lock( &m_brkMutex );

    for (iter = m_mWorkerTable.begin(); iter != m_mWorkerTable.end(); iter++ ) {
        ITF_DP_Client *clt = iter->second;
        if ( (clt->m_fname.find(WORKER_TYPE[type]) != string::npos) && (!clt->m_state) ) {
            clt->m_state = 1;
            fname = clt->m_fname;
            break;
        }
    }

    pthread_mutex_unlock( &m_brkMutex );
    return fname;
}

int ITF_DP_Broker::getWorkerCount(int type)
{
    string fname="";
    int ret=0;
    map< string, ITF_DP_Client* >::iterator iter;

    if (type>3) return ret;

    for (iter = m_mWorkerTable.begin(); iter != m_mWorkerTable.end(); iter++ ) {
        ITF_DP_Client *clt = iter->second;
        if ( (clt->m_fname.find(WORKER_TYPE[type]) != string::npos) ) {
            ret++;
        }
    }

    return ret;
}