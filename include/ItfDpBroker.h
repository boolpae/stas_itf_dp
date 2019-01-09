#ifndef _ITF_DP_BROKER_H_
#define _ITF_DP_BROKER_H_

#include <map>
#include <string>
#include <thread>

using namespace std;

#define SOCKET int

/*
 * ITF_DP_Broker
 * by boolpae
 * 
 * Desc:
 * Gearman을 대신하여 stas에서 실시간 STT를 위해 VR과 직접 연결(TCP) 및 데이터 송수신을 위해 사용
 * 기본 tcp/7100 포트를 이용하여 VR에서의 연결을 기다리며 연결이 완료되면 자체 테이블에 client socket
 * 을 등록하여 추후 VRClient 에서 사용할 수 있도록 관리한다.
 * 
 * VR에서 접속되는 순서대로 번호를 배당하여 vr_realtime_n 형식으로 관리한다. - [ string | socket_no ]
 */

class ITF_DP_Broker;
class ITF_DP_Client {
    public:
    ITF_DP_Broker *m_brk;
    string m_fname;
    int m_state;    // 0: init(대기), 1: working(사용중), 2: error(폐기 예정)
    SOCKET m_sockfd;

    private:
    char *m_pTransBuff;
    int m_nValueSize;
    int m_nTransBuffSize;

    public:
    ITF_DP_Client(ITF_DP_Broker *brk, string fname, SOCKET sockfd);
    virtual ~ITF_DP_Client();

    int requestJob(void *data, int len);
    int getValueSize() { return m_nValueSize; }
    char *getValue() { return m_pTransBuff; }

    private:
    static void thrdKeepAlive(ITF_DP_Client *wrk);
};

class ITF_DP_Broker {
    public:
    static ITF_DP_Broker* instance(int port);
    static void release();

    void eraseWorker(string fname);

    // reserve 시 반환된 fname을 이용하여 request 한다.
    ITF_DP_Client* requestWorker(string fname);

    // restore 시엔 ITF_DP_Broker의 pointer를 활용한다. 더불어 mutex를 이용하여 thred-safty하게 설계한다.
    void restoreWorker(ITF_DP_Client *wrk);
    void restoreWorker(string fname);

    // reserve 시에 mutex를 사용하여 다른 쓰레드가 reserve 할 수 없게 한다.
    // type: 0-realtime, 1-stt, 2-unseg
    string reserveWorker(int type);

    void outputTableState();

    int getWorkerCount(int type);

    private:
    ITF_DP_Broker(int sock);
    virtual ~ITF_DP_Broker();

    static void thrdListner(ITF_DP_Broker *brk);
#if 0
    static void thrdCheckClientSocket(ITF_DP_Broker *brk);
#endif

    private:
    static ITF_DP_Broker* m_instance;
    
    map< string, ITF_DP_Client* > m_mWorkerTable;

    int m_nMainSocket;

    pthread_mutex_t	 m_brkMutex;

};





#endif // _ITF_DP_BROKER_H_