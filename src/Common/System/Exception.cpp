#include "System/Exception.h"

#if defined (_WIN64)

CWindowsException::CWindowsException(const CWindowsException& other)
{
	m_code = other.m_code;
	if (other.m_description.empty())
		ResolveDescription();
	else
		m_description = other.m_description;
}

CWindowsException& CWindowsException::operator = (const CWindowsException& other)
{
	if (this != &other)
	{
		m_code = other.m_code;
		if (other.m_description.empty())
			ResolveDescription();
	}

	return *this;
}

const char* CWindowsException::what() const
{
	return m_description.c_str();
}

std::string CWindowsException::GetErrorDescription(ULONG code)
{
	_TCHAR* msg = nullptr;
	std::string descr;

	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, 
		nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<_TCHAR*>(&msg), 0, nullptr);
	if (msg)
	{
		_tstringstream stm;
		stm << _T("Windows error ") << code << _T(" - ") << msg;
		_tstring str = stm.str();
		descr.assign(std::begin(str), std::end(str));
		LocalFree(msg);
	}

	return descr;
}

void CWindowsException::ResolveDescription()
{
	m_description = GetErrorDescription(m_code);
}

_se_translator_function CSehException::s_prev = nullptr;

void CSehException::Setup()
{
	s_prev = _set_se_translator(&CSehException::Handler);
}

void CSehException::Remove()
{
	_set_se_translator(s_prev);
	s_prev = nullptr;
}

CSehException::CSehException(unsigned int code, PEXCEPTION_POINTERS extraData)
: m_code(code)
, m_addr(extraData->ExceptionRecord->ExceptionAddress)
{
	ResolveDescription();
}

CSehException::CSehException(const CSehException& other)
{
	m_code = other.m_code;
	if (other.m_description.empty())
		ResolveDescription();
	else
		m_description = other.m_description;
}

CSehException& CSehException::operator =(const CSehException& other)
{
	if (this != &other)
	{
		m_code = other.m_code;
		if (other.m_description.empty())
			ResolveDescription();
	}

	return *this;
}

void CSehException::Handler(unsigned int code, PEXCEPTION_POINTERS extraData)
{
	throw CSehException(code, extraData);
}

const char* CSehException::what() const
{
	return m_description.c_str();
}

void CSehException::ResolveDescription()
{
	std::stringstream stm;
	stm << "SEH error " << m_code << " at address " << m_addr << "." <<std::endl;
	m_description = stm.str();
}

#endif //_WIN64

std::string SystemException::GetErrorDescription(int err)
{
	return std::string(strerror(err));
}

SystemException::SystemException(int err) : m_err(err) {}
    
SystemException::SystemException(const SystemException& other)
{
	m_err = other.m_err;
}

SystemException& SystemException::operator= (const SystemException& other)
{
	if (this != &other)
	{
		m_err = other.m_err;
	}

	return *this;
}

const char* SystemException::what() const noexcept
{
	return SystemException::GetErrorDescription(m_err).c_str();
}