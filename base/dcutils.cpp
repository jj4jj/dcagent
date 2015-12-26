#include "dcutils.hpp"
#include "logger.h"

namespace dcsutil {
    uint64_t	time_unixtime_us(){
        timeval tv;
        gettimeofday(&tv, NULL);
        return tv.tv_sec * 1000000 + tv.tv_usec;
    }

    int			daemonlize(int closestd, int chrootdir){
#if _BSD_SOURCE || (_XOPEN_SOURCE && _XOPEN_SOURCE < 500)
        return daemon(!chrootdir, !closestd);
#else
        assert("not implement in this platform , using nohup & launch it ?")
            return -404;//todo 
#endif
    }
    int			readfile(const std::string & file, char * buffer, size_t sz){
        FILE * fp = fopen(file.c_str(), "r");
        if (!fp){
            GLOG_ERR("open file��%s error:%d", file.c_str(), errno);
            return -1;
        }
        if (sz == 0 || !buffer){
            return 0; //file exist
            fclose(fp);
        }
        int n;
        size_t tsz = 0;
        while ((n = fread(buffer + tsz, 1, sz - tsz, fp))){
            if (n > 0){
                tsz += n;
            }
            else if (errno != EINTR &&
                errno != EAGAIN && errno != EWOULDBLOCK) {
                GLOG_ERR("read file:%s ret:%d error :%d total sz:%zu", file.c_str(), n, errno, tsz);
                break;
            }
        }
        fclose(fp);
        if (n >= 0){
            if (tsz < sz){
                buffer[tsz] = 0;
            }
            return tsz;
        }
        else {
            return -2;
        }
    }
    size_t      filesize(const std::string & file){
        FILE * fp = fopen(file.c_str(), "r");
        if (!fp){
            return 0;//not exist 
        }
        fseek(fp, 0L, SEEK_END);
        size_t sz = ftell(fp);
        fclose(fp);
        return sz;
    }
    int			writefile(const std::string & file, const char * buffer, size_t sz){
        FILE * fp = fopen(file.c_str(), "w");
		if (!fp){
            GLOG_ERR("open file��%s error:%d", file.c_str(), errno);
			return -1;
		}
		if (sz == 0){
			sz = strlen(buffer);
		}
		size_t tsz = 0;
		int n = 0;
		while ((n = fwrite(buffer + tsz, 1, sz - tsz, fp))){
			if (n > 0){
				tsz += n;
			}
			else  if (errno != EINTR &&
				errno != EAGAIN && errno != EWOULDBLOCK) {
                GLOG_ERR("write file:%s ret:%d error :%d writed sz:%zu total:%zu", file.c_str(), n, errno, tsz, sz);
				break;
			}
		}
		fclose(fp);
		if (tsz == sz){
			return tsz;
		}
		else {
            GLOG_ERR("write file:%s writed:%zu error :%d total sz:%zu", file.c_str(), tsz, errno, sz);
			return -2;
		}
	}
    //file://<path>
    //tcp://<ip:port>
    //udp://<ip:port>
    int                 openfd(const std::string & uri, int timeout_ms){
        if (uri.find("file://") == 0){ //+7
            return open(uri.substr(7).c_str(), O_CREAT | O_RDWR);
        }
        else if (uri.find("tcp://") == 0){//+6
            std::vector<string> vs;
            const char * connaddr = uri.substr(6).c_str();
            strsplit(connaddr, ":", vs);
            if (vs.size() != 2){
                GLOG_ERR("error tcp address:%s", connaddr);
                return -1;
            }
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0){
                GLOG_ERR("create sock stream error !");
                return -2;
            }
            sockaddr_in iaddr;
            iaddr.sin_family = AF_INET;
            iaddr.sin_addr.s_addr = inet_addr(vs[0].c_str());
            iaddr.sin_port = htons(stoi(vs[1]));
            socklen_t len = sizeof(sockaddr_in);
            nonblockfd(fd, true);
            int ret = connect(fd, (struct sockaddr*)&iaddr, len);
            if (ret){
                if (errno == EINPROGRESS){
                    ret = waitfd_writable(fd, timeout_ms);
                }
                if (fd){
                    GLOG_ERR("connect addr:%s error !", connaddr);
                    close(fd);
                    return -3;
                }
            }
            return fd;
        }
        else if (uri.find("udp://") == 0){//+6
            std::vector<string> vs;
            const char * connaddr = uri.substr(6).c_str();
            strsplit(connaddr, ":", vs);
            if (vs.size() != 2){
                GLOG_ERR("error tcp address:%s", connaddr);
                return -1;
            }
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0){
                GLOG_ERR("create sock stream error !");
                return -2;
            }
            sockaddr_in iaddr;
            iaddr.sin_family = AF_INET;
            iaddr.sin_addr.s_addr = inet_addr(vs[0].c_str());
            iaddr.sin_port = htons(stoi(vs[1]));
            socklen_t len = sizeof(sockaddr_in);
            int ret = connect(fd, (struct sockaddr*)&iaddr, len);
            if (ret){
                GLOG_ERR("connect addr:%s error !", connaddr);
                close(fd);
                return -3;
            }
            return fd;
        }
        else {
            GLOG_ERR("not support uri:%s", uri.c_str());
            return -1;
        }
        return -2;
    }
    static inline size_t _readfd(int fd, char * buffer, size_t sz, int timeout_ms){
        int n;
        size_t tsz = 0;
        while (sz > tsz && (n = read(fd, buffer + tsz, sz - tsz))){
            if (n > 0){
                tsz += n;
            }
            else if (errno != EINTR){
                if (errno == EAGAIN || errno == EWOULDBLOCK){//
                    if (waitfd_readable(fd, timeout_ms)){
                        GLOG_ERR("read fd:%d time out:%dms tsz:%zd", fd, timeout_ms, tsz);
                        return -1;
                    }
                }
                else {
                    GLOG_ERR("read fd:%d ret:%d error :%d total sz:%zd", fd, n, errno, tsz);
                    return -2;
                }
            }
        }
        return tsz;
    }
    //mode: size, end, msg:sz32/16/8, token:\r\n\r\n , return > 0 read size, = 0 end, < 0 error
    int                 readfd(int fd, char * buffer, size_t sz, const char * mode, int timeout_ms){
        if (sz == 0 || !buffer){
            return 0;
        }
        if (strstr(mode, "size")){
            //just read sz or end
            size_t tsz = _readfd(fd, buffer, sz, timeout_ms);
            if (sz != tsz){
                GLOG_ERR("read fd:%d size sz:%zd tsz:%zd not match !", fd, sz, tsz);
                return -1;
            }
            return tsz;
        }
        else if (strstr(mode, "end")){
            return _readfd(fd, buffer, sz, timeout_ms);
        }
        else if (strstr(mode, "msg:")){
            //read 32
            int    nrsz = 0;
            int szlen = 0;
            if (strstr(mode, ":sz32")){
                szlen = sizeof(uint32_t);
            }
            else if (strstr(mode, ":sz16")){
                szlen = sizeof(uint16_t);
            }
            else if (strstr(mode, ":sz8")){
                szlen = sizeof(uint8_t);
            }
            else {
                GLOG_ERR("read fd:%d error mode:%s",fd, mode);
                return -1;
            }

            if (readfd(fd, (char *)&nrsz, szlen, "size") != szlen){
                GLOG_ERR("read fd:%d size16 head error !", fd);
                return -2;
            }
            if (szlen == 16){
                nrsz = ntohs(nrsz);
            }
            else if (szlen == 32){
                nrsz = ntohl(nrsz);
            }
            if (sz < nrsz){
                GLOG_ERR("read fd:%d buffer size:%d not enough %zd  ", fd, nrsz, sz);
                return -3;
            }
            if (readfd(fd, buffer, nrsz, "size") != nrsz){
                GLOG_ERR("read fd:%d buffer data erorr size:%d", fd, nrsz);
                return -4;
            }
            return nrsz;
        }
        else if (strstr(mode, "token:")){
            const char * sep = mode + 6;
            if (!*sep){ //blocksz == 0
                GLOG_ERR("error mode:%s", mode);
                return -1;
            }
            int nrsz = 0;
            int seplen = strlen(sep);
            int blocksz = strlen(sep);
            int rdz = 0;
            int matchnum = 0;
            while (true){
                rdz = readfd(fd, buffer + nrsz, blocksz, "end");
                if (rdz < 0){
                    GLOG_ERR("read fd:%d ret:%d error !", fd, rdz);
                    return rdz;
                }
                nrsz += rdz;
                //=============
                if (rdz == 0){
                    return nrsz;
                }
                matchnum = 0; //matched num
                while (matchnum < seplen && *(buffer + nrsz - rdz + matchnum) == *(sep + matchnum)){
                    ++matchnum;
                }
                if (matchnum == seplen){ //matched all
                    return nrsz;
                }
                blocksz = seplen - matchnum;
            }
        }
        else {
            GLOG_ERR("not support mode:%s", mode);
            return -1;
        }
    }
    //write size , return > 0 wirte size, <= 0 error
    int         writefd(int fd, const char * buffer, size_t sz, int timeout_ms){
        if (sz == 0){
            sz = strlen(buffer);
        }
        size_t tsz = 0;
        int n = 0;
        while (sz > tsz && (n = write(fd, buffer + tsz, sz - tsz))){
            if (n > 0){
                tsz += n;
            }
            else  if (errno != EINTR){
                if (errno == EAGAIN || errno == EWOULDBLOCK){
                    if (waitfd_readable(fd, timeout_ms)){
                        GLOG_ERR("write fd:%d time out:%dms tsz:%zd", fd, timeout_ms, tsz);
                        return -1;
                    }
                }
            }
        }
        return tsz;
    }
    int        closefd(int fd){
        int ret = close(fd);
        if (ret){
            GLOG_ERR("close fd:%d error !", fd);
        }
        return ret;
    }
    int         nonblockfd(int fd, bool nonblock){
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0){
            return -1;
        }
        if (nonblock){
            flags |= O_NONBLOCK;
        }
        else{
            flags &= ~(O_NONBLOCK);
        }
        if (fcntl(fd, F_SETFL, flags) < 0){
            return -1;
        }
        return 0;
    }
    //return 0: readable , other error occurs
    int         waitfd_readable(int fd, int timeout_ms){
        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(fd, &fdset);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000 ) * 1000;
        int ret = select(fd + 1, &fdset, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(fd, &fdset)){
            return 0;
        }
        return ret;
    }
    int         waitfd_writable(int fd, int timeout_ms){
        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(fd, &fdset);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int ret = select(fd + 1, NULL, &fdset, NULL, &tv);
        if (ret > 0 && FD_ISSET(fd, &fdset)){
            return 0;
        }
        return ret;
    }
    int			lockpidfile(const std::string & file, int kill_other_sig, bool nb){
        int fd = open(file.c_str(), O_RDWR | O_CREAT, 0644);
		if (fd == -1) {
            GLOG_ERR("open file:%s error ", file.c_str());
			return -1;
		}
		int flags = LOCK_EX;
		if (nb){
			flags |= LOCK_NB;
		}
		char szpid[16] = { 0 };
		int pid = 0;
		while (flock(fd, flags) == -1) {
			if (pid == 0){ //just read once
                int n = readfile(file, szpid, sizeof(szpid));
				if (n > 0){
					pid = strtol(szpid, NULL, 10);
                    GLOG_ERR("lock pidfile:%s fail , the file is held by pid %d", file.c_str(), pid);
				}
				else {
                    GLOG_ERR("lock pidfile:%s fail but read pid from file error !", file.c_str());
				}
			}
			if (pid > 0 && kill_other_sig > 0){
				if (kill(pid, kill_other_sig) && errno == ESRCH){
                    GLOG_WAR("killed the pidfile locker:%d by signal:%d", pid, kill_other_sig);
					break;
				}
			}
			else {
				return pid;
			}
		}
		pid = getpid();
		snprintf(szpid, sizeof(szpid), "%d", pid);
        writefile(file, szpid);

		return pid;
	}
    int			strsplit(const std::string & str, const string & sep, std::vector<std::string> & vs,
                        bool ignore_empty, int maxsplit, int beg_ , int end_ ){
		vs.clear();
        string::size_type beg = beg_;
        string::size_type end = str.length();
        if (end_ > 0){
            end = end_;
        }
		string::size_type pos = 0;
		//if pos not found add the rest then return , else add substr . again
		do {
			pos = str.find(sep, beg);
			if ( pos != string::npos && pos < end ){
				if (pos > beg){
					vs.push_back(str.substr(beg, pos - beg));
				}
				else if (!ignore_empty){
					vs.push_back(""); //empty 
				}
				beg = pos + sep.length();
			}
			if (pos == string::npos || //last one
				(maxsplit > 0 && (int)vs.size() + 1 == maxsplit) ||
                pos > end){
				if (beg < end){
					vs.push_back(str.substr(beg, end - beg));
				}
				else if (!ignore_empty){
					vs.push_back(""); //empty 
				}
				return vs.size();
			}
		} while (true);
		return vs.size();
	}
	const char*		strftime(std::string & str, time_t unixtime, const char * format){
		str.reserve(32);
		if (unixtime == 0U){
			unixtime = time(NULL);
		}
		struct tm _sftm;
		localtime_r(&unixtime, &_sftm);
		strftime((char*)str.c_str(), str.capacity(), format, &_sftm);
		return str.c_str();
	}
    const char*         strptime(time_t & unixtime ,const std::string & str, const char * format){
        struct tm _tmptm;
        unixtime = 0;
        const char * p = strptime(str.c_str(), format, &_tmptm);
        if(!p){
            return nullptr;
        }
        unixtime = mktime(&_tmptm);
        return p;
    }
	time_t			    stdstrtime(const char * strtime){
#if 0
		int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
		sscanf(strtime, "%4d-%2d-%2dT%02d:%02d:%02d", &Y, &M, &D, &h, &m, &s);
		struct tm stm;
		stm.tm_year = Y - 1900;
		stm.tm_mon = M - 1;
		stm.tm_mday = D;
		stm.tm_hour = h;
		stm.tm_min = m;
		stm.tm_sec = s;
		stm.tm_isdst = 0;
		return mktime(&stm);
 #else
        time_t _tmt;
        strptime(_tmt, strtime, "%Y-%m-%dT%H:%M:%S");
        return _tmt;
 #endif
	}
    bool            strisint(const std::string & str, int base){
        char * endptr;
        auto v = strtoll(str.c_str(), &endptr, base);
        if (endptr == str.c_str()){
            return false;
        }
        if (v == LLONG_MAX || v == LLONG_MIN){
            return false;
        }
        return true;
    }
	void			strrepeat(std::string & str, const char * rep, int repcount){
		while (repcount-- > 0){
			str.append(rep);
		}
	}
	size_t			vstrprintf(std::string & str, const char* format, va_list ap){
		size_t ncvt = vsnprintf((char*)str.data(), str.capacity(), format, ap);
		if (ncvt == str.capacity()){
			str[ncvt - 1] = 0;
			--ncvt;
		}
		return ncvt;
	}
	size_t			strprintf(std::string & str, const char * format, ...){
		va_list	ap;
		va_start(ap, format);
		size_t ncvt = vstrprintf(str, format, ap);
		va_end(ap);
		return ncvt;
	}
	size_t			strnprintf(std::string & str, size_t max_sz, const char * format, ...){
		str.reserve(max_sz);
		va_list	ap;
		va_start(ap, format);
		size_t ncvt = vstrprintf(str, format, ap);
		va_end(ap);
		return ncvt;
	}


	//////////////////////////////////////////////////////////////////////////////////////////
    const char*			strcharsetrandom(std::string & randoms, int length , const char * charset){
        if (!charset || !(*charset)){
            return nullptr;
        }
        int charsetlen = strlen(charset);
        std::random_device	rd;
        for (int i = 0; i < length; ++i){
            randoms.append(1, charset[rd() % charsetlen]);
        }
        return randoms.c_str();
    }
    const char*			strrandom(std::string & randoms, int length, char charbeg, char charend){
		if (charbeg > charend){std::swap(charbeg, charend);}
		std::random_device	rd;
		for (int i = 0; i < length; ++i){
			randoms.append(1, (char)(rd() % (charend - charbeg + 1) + charbeg));
		}
		return randoms.c_str();
	}

    int                 strsunpack(const std::string & str, const std::string & sep, const std::string & ks, ...){
        //{K=V,}
        if (str[0] != '{' || *str.rbegin() != '}'){
            return -1;//valid format
        }
        //name=v
        std::map<string, string>      kvmap;
        std::vector<string> vs;
		string kvsep;
		strrepeat(kvsep, sep.c_str(), 2);//,
		strsplit(str, kvsep, vs, true, 0, 1, str.length() - 1); //{<---------->}
        for (auto & kv : vs){
            std::vector<string> skv;
            int lkv = strsplit(kv, sep, skv, true, 2);//k=v
            if (lkv == 2){
                kvmap[skv[0]] = skv[1];
            }
        }
		/////////////////////////////////////////
		std::vector<string> vsks;
		strsplit(ks, ",", vsks, true);
		va_list ap;
		va_start(ap, ks);
		for (auto & k : vsks){
			//k=v
			string * v = va_arg(ap, std::string *);
			v->assign(kvmap[k]);
		}
		va_end(ap);
        return vs.size();
    }
    const char         *strspack(std::string & str, const std::string & sep, const std::string & ks, ...){
        //{K=V,}
        str = "{";
        //name=v
        std::vector<string> vs;
        strsplit(ks, ",", vs, true);
        va_list ap;
        va_start(ap, ks);
		for (size_t i = 0; i < vs.size(); ++i){
            if (i > 0){
                strrepeat(str, sep.c_str(), 2);//,
            }
            //k=v
            str += vs[i];//k
            const string * v = va_arg(ap, std::string *);
            strrepeat(str, sep.c_str(), 1);//=
            str += *v;
        }
        va_end(ap);
        str += "}";
        return str.data();
    }











}



