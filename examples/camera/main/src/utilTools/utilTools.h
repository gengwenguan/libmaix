/********************************************************************************* 
  *Copyright(C),Your Company 
  *FileName:  utiltools.h
  *Author:    gengwenguan
  *Date:      2024-10-25
  *Description:  通用工具接口
**********************************************************************************/ 
#pragma once
#include<mutex>
#include<memory>
#include<condition_variable>

//读锁持续获取时，可能会导致获取写锁饥饿问题
class C_RWLock 
{
public:
	C_RWLock():readers(0),writing(false){}

	/* 读锁 */
	void RLock(){
		std::unique_lock<std::mutex> lock(mutex_);
		while (writing){
			condition_.wait(lock);
		}
		++readers;
	}

	/* 读解锁 */
	void RUnlock(){
		std::unique_lock<std::mutex> lock(mutex_);
		--readers;
		if (readers == 0){
			condition_.notify_one();
		}
	}

	/* 写锁 */
	void WLock(){
		std::unique_lock<std::mutex> lock(mutex_);
		while (writing || readers > 0){
			condition_.wait(lock);
		}
		writing = true;
	}

	/* 写解锁 */
	void WUnlock(){
		std::unique_lock<std::mutex> lock(mutex_);
		writing = false;
		condition_.notify_all();
	}

private:
	std::mutex mutex_;
	std::condition_variable condition_;
	int readers;
	bool writing;
};

/* 自动读锁定，离开作用域自动解锁 */
class C_AutoReadGuard
{
public:
	C_AutoReadGuard(C_RWLock &objRWLock)
		:m_objRLock(objRWLock){
		m_objRLock.RLock();
	}

	~C_AutoReadGuard(){
		m_objRLock.RUnlock();
	}

private:
	C_RWLock& m_objRLock;
};

/* 自动写锁定，离开作用域自动解锁 */
class C_AutoWriteGuard
{
public:
	C_AutoWriteGuard(C_RWLock &objRWLock)
		:m_objWLock(objRWLock){
		m_objWLock.WLock();
	}

	~C_AutoWriteGuard(){
		m_objWLock.WUnlock();
	}

private:
	C_RWLock &m_objWLock;
};


/* 锁和条件变量配合实现信号量 */
class C_Semaphore 
{
public:
	C_Semaphore(int value = 0) : count{ value } { }

	void wait() {
		std::unique_lock <std::mutex > lock{ mutex };
		condition.wait(lock, [this](){ return count > 0; });
		--count;
	}
	void  signal() {
		{
			std::lock_guard <std::mutex > lock{ mutex };
			++count;
		}
		condition.notify_one();  // notify one !
	}
	int GetCount() { return count; }

private:
	int count;
	std::mutex mutex;
	std::condition_variable condition;
};

//字符串格式化输出，可控制输出之间的间隔,需保证pBuf空间足够大足够容纳格式化打印的数据
//传入的pBuf可以是带有字符串的地址，该类会在字符串后面追加数据
//使用示例
/*
	char buff[10240];
	//pair第一项为占用长度，第二项为格式进行初始化
	C_FormatPrint obj(buff, { { 12,"%s" },{ 12,"%d:%d" },{ 12,"%s" } });
	obj.Print("ert").Print(123, 456).Print("zxcv");
	printf("%s", buff);
	//输出ert         123:456     zxcv

	//使用标头字符串进行初始化，后续打印的变量起始位置会自动和标头字符串对齐
	std::string str = "   type       ssrc:chg            pt       \n";
	sprintf(buff + strlen(buff), "%s", str.c_str());
	C_FormatPrint obj1(buff, str, { "%s","%d:%d","%d" });
	obj1.Print("video").Print(12345, 3).Print(96);
	obj1.Refresh();
	obj1.Print("audio").Print(456, 8).Print(112);
	printf("%s", buff);
	//输出如下
		type       ssrc:chg            pt
		video      12345:3             96
		audio      456:8               112
*/

class C_FormatPrint {
public:
	//使用每个字串占位宽带来初始化
	C_FormatPrint(char *pBuf, std::vector<std::pair<int, std::string>> formatspair)
	{
		ResetFormat(pBuf, formatspair);
	}
	void ResetFormat(char *pBuf, std::vector<std::pair<int, std::string>> formatspair) {
		m_Pos.clear();
		m_formats.clear();
		m_idx = 0;
		int pos = 0;
		for (auto pair : formatspair) {               //根据每项输出的宽度计算每项输出的位置
			m_Pos.push_back(pos);
			m_formats.push_back(pair.second);
			pos += pair.first;
		}
		m_pBuf = pBuf + strlen(pBuf);              //之前的字符串保留，再之前字符串尾部追加打印
		memset(m_pBuf, ' ', pos);                  // 将要格式化输出的内存区域置为空格
		*(m_pBuf + pos) = '\0';                    // 最后设置文本结束符
	}

	//使用输出头字符串进行初始化，从header中找到每个子串的起始位置作为后续变量输出的位置
	C_FormatPrint(char *pBuf, std::string header, std::vector<std::string> formats) {
		ResetFormat(pBuf, header, formats);
	}
	void ResetFormat(char *pBuf, std::string header, std::vector<std::string> formats) {
		m_idx = 0;
		m_formats = formats;
		bool in_word = false;
		for (size_t i = 0; i < header.size(); ++i) {
			if (header[i] != ' ' && header[i] != '\n') {
				if (!in_word) {
					m_Pos.push_back(i);
					in_word = true;
				}
			}
			else {
				in_word = false;
			}
		}
		// 数量要匹配
		if (m_formats.size() == m_Pos.size()) {
			m_pBuf = pBuf + strlen(pBuf);              //之前的字符串保留，再之前字符串尾部追加打印
			memset(m_pBuf, ' ', m_Pos.back());         // 将要格式化输出的内存区域置为空格
			*(m_pBuf + m_Pos.back()) = '\0';           // 最后设置文本结束符

		}
	}

	//刷新后，可按照之前配置的格式再次打印
	void Refresh() {
		m_idx = 0;
		m_pBuf = m_pBuf + strlen(m_pBuf);              //之前的字符串保留，再之前字符串尾部追加打印
		memset(m_pBuf, ' ', m_Pos.back());             // 将要格式化输出的内存区域置为空格
		*(m_pBuf + m_Pos.back()) = '\0';               // 最后设置文本结束符
	}

	//必须保证Print调用的次数和传入的formats数据个数相同。打印string类型变量时要使用c_str()传入
	template<typename... Args>
	C_FormatPrint& Print(const Args&... args) {
		if (m_idx >= m_formats.size()) return *this;
		if (m_formats.size() != m_Pos.size()) return *this;

		// 格式化输出
		sprintf(m_pBuf + m_Pos[m_idx], m_formats[m_idx].c_str(), args...);

		// 处理结尾字符
		char* end = m_pBuf + strlen(m_pBuf);
		if (m_idx < m_formats.size() - 1) {
			*end = ' ';  // 非最后一项用空格覆盖结尾的\0
		}
		else {
			sprintf(end, "\n");  // 最后一项追加换行
		}
		m_idx++;
		return *this;
	}

private:
	char *m_pBuf;
	unsigned int m_idx;
	std::vector<int> m_Pos;              //每一项的起始位置
	std::vector<std::string> m_formats;  //每一项的输出格式
};
