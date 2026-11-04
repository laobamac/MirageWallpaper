#pragma once

#include <QProcess>
#include <QString>
#ifdef Q_OS_WIN
#include <windows.h>
#include <memory>
#include <vector>
#endif

namespace Mirage {

// 创建时分配 Job，避免子进程在 started 信号之前派生不受管理的孙进程。
inline bool configureProcessJob(QProcess* process, QString* error = nullptr) {
#ifdef Q_OS_WIN
    struct Job {
        HANDLE handle = nullptr;
        std::vector<unsigned char> attributes;
        STARTUPINFOEXW startup{};
        bool initialized = false;
        ~Job() {
            if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
            if (handle) CloseHandle(handle);
        }
        void close() {
            if (handle) {
                CloseHandle(handle);
                handle = nullptr;
            }
        }
    };
    const auto job = std::make_shared<Job>();
    auto failed = [error](DWORD code) {
        if (error) *error = QStringLiteral("无法配置 Windows 子进程 Job（错误 %1）").arg(code);
        return false;
    };
    job->handle = CreateJobObjectW(nullptr, nullptr);
    if (!job->handle) return failed(GetLastError());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job->handle, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return failed(GetLastError());
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    job->attributes.resize(size);
    job->startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(job->attributes.data());
    if (!InitializeProcThreadAttributeList(job->startup.lpAttributeList, 1, 0, &size))
        return failed(GetLastError());
    job->initialized = true;
    if (!UpdateProcThreadAttribute(job->startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                   &job->handle, sizeof(job->handle), nullptr, nullptr))
        return failed(GetLastError());
    process->setCreateProcessArgumentsModifier([job](QProcess::CreateProcessArguments* args) {
        job->startup.StartupInfo = *args->startupInfo;
        job->startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        args->startupInfo = &job->startup.StartupInfo;
        args->flags |= EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW;
    });
    QObject::connect(process, &QProcess::finished, process,
                     [job](int, QProcess::ExitStatus) { job->close(); }, Qt::SingleShotConnection);
#else
    Q_UNUSED(process)
    Q_UNUSED(error)
#endif
    return true;
}

} // namespace Mirage
