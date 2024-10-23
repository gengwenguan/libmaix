#include<mutex>
#include<memory>
#include<condition_variable>

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