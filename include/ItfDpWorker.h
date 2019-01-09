
#ifndef _ITF_DP_WORKER_H_
#define _ITF_DP_WORKER_H_

#include <string>

using namespace std;

#define SOCKET int

class ITF_DP_Worker {
    private:
    string m_sBrokerIp;
    int m_nBrokerPort;
    SOCKET m_sockfd;
    char *m_pTransBuff;
    int m_nTransBuffSize;
    int m_nValueSize;

    public:
    string m_sType;
    string m_sWorkerID;
    
    public:
    ITF_DP_Worker(string ipaddr, int port);
    virtual ~ITF_DP_Worker();

    int init(string type);

    int getValueSize() { return m_nValueSize; }
    char *getValue() { return m_pTransBuff; }

    int waitJob();
    int responseJob(void *data, int len);

    private:
};

#endif // _WORKER_H_