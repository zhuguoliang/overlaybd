/*
   Copyright The Overlaybd Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
#include <errno.h>
#include <list>
#include <set>
#include <string>
#include <sys/file.h>
#include "overlaybd/alog-stdstring.h"
#include "overlaybd/alog.h"
#include "overlaybd/fs/localfs.h"
#include "overlaybd/fs/throttled-file.h"
#include "overlaybd/photon/thread.h"
#include "bk_download.h"
#include "overlaybd/event-loop.h"
using namespace FileSystem;

static constexpr size_t ALIGNMENT = 4096;

namespace BKDL {

bool check_downloaded(const std::string &dir) {
    std::string fn = dir + "/" + COMMIT_FILE_NAME;
    auto lfs = FileSystem::new_localfs_adaptor();
    if (!lfs) {
        LOG_ERROR("new_localfs_adaptor() return NULL");
        return false;
    }
    DEFER({ delete lfs; });

    if (lfs->access(fn.c_str(), 0) == 0)
        return true;
    return false;
}

static std::set<std::string> lock_files;

ssize_t filecopy(IFile *infile, IFile *outfile, size_t bs, int retry_limit, int &running) {
    if (bs == 0)
        LOG_ERROR_RETURN(EINVAL, -1, "bs should not be 0");
    void *buff = nullptr;

    // buffer allocate, with 4K alignment
    ::posix_memalign(&buff, ALIGNMENT, bs);
    if (buff == nullptr)
        LOG_ERROR_RETURN(ENOMEM, -1, "Fail to allocate buffer with ", VALUE(bs));
    DEFER(free(buff));
    off_t offset = 0;
    ssize_t count = bs;
    while (count == (ssize_t)bs) {
        if (running != 1) {
            LOG_INFO("image file exit when background downloading");
            return -1;
        }

        int retry = retry_limit;
    again_read:
        if (!(retry--))
            LOG_ERROR_RETURN(EIO, -1, "Fail to read at ", VALUE(offset), VALUE(count));
        auto rlen = infile->pread(buff, bs, offset);
        if (rlen < 0) {
            LOG_DEBUG("Fail to read at ", VALUE(offset), VALUE(count), " retry...");
            goto again_read;
        }
        retry = retry_limit;
    again_write:
        if (!(retry--))
            LOG_ERROR_RETURN(EIO, -1, "Fail to write at ", VALUE(offset), VALUE(count));
        // cause it might write into file with O_DIRECT
        // keep write length as bs
        auto wlen = outfile->pwrite(buff, bs, offset);
        // but once write lenth larger than read length treats as OK
        if (wlen < rlen) {
            LOG_DEBUG("Fail to write at ", VALUE(offset), VALUE(count), " retry...");
            goto again_write;
        }
        count = rlen;
        offset += count;
    }
    // truncate after write, for O_DIRECT
    outfile->ftruncate(offset);
    return offset;
}

void BkDownload::switch_to_local_file() {
    std::string path = dir + "/" + COMMIT_FILE_NAME;
    ((ISwitchFile *)sw_file)->set_switch_file(path.c_str());
    LOG_DEBUG("set switch tag done. (localpath: `)", path);
}

bool BkDownload::download_done() {
    auto lfs = new_localfs_adaptor();
    if (!lfs) {
        LOG_ERROR("new_localfs_adaptor() return NULL");
        return false;
    }
    DEFER({ delete lfs; });

    std::string old_name, new_name;
    old_name = dir + "/" + DOWNLOAD_TMP_NAME;
    new_name = dir + "/" + COMMIT_FILE_NAME;

    int ret = lfs->rename(old_name.c_str(), new_name.c_str());
    if (ret != 0) {
        LOG_ERROR("rename(`,`), `:`", old_name, new_name, errno, strerror(errno));
        return false;
    }
    LOG_INFO("rename(`,`) success", old_name, new_name);
    return true;
}

bool BkDownload::download(int &running) {
    if (check_downloaded(dir)) {
        switch_to_local_file();
        return true;
    }

    if (download_blob(running)) {
        if (!download_done())
            return false;
        switch_to_local_file();
        return true;
    }
    return false;
}

bool BkDownload::lock_file() {
    if (lock_files.find(dir) != lock_files.end()) {
        LOG_WARN("failded to lock download path:`", dir);
        return false;
    }
    lock_files.insert(dir);
    return true;
}

void BkDownload::unlock_file() {
    lock_files.erase(dir);
}

bool BkDownload::download_blob(int &running) {
    std::string dl_file_path = dir + "/" + DOWNLOAD_TMP_NAME;
    try_cnt--;
    FileSystem::IFile *src = src_file;
    if (limit_MB_ps > 0) {
        FileSystem::ThrottleLimits limits;
        limits.R.throughput = limit_MB_ps * 1024UL * 1024; // MB
        limits.R.block_size = 1024UL * 1024;
        limits.time_window = 1UL;
        src = FileSystem::new_throttled_file(src, limits);
    }
    DEFER({
        if (limit_MB_ps > 0)
            delete src;
    });

    auto dst = FileSystem::open_localfile_adaptor(dl_file_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (dst == nullptr) {
        LOG_ERRNO_RETURN(0, -1, "failed to open dst file `", dl_file_path.c_str());
    }
    DEFER(delete dst;);

    auto res = filecopy(src, dst, 1024UL * 1024, 1, running);
    if (res < 0)
        return false;
    return true;
}

void bk_download_proc(std::list<BKDL::BkDownload *> &dl_list, uint64_t delay_sec, int &running) {
    LOG_INFO("BACKGROUND DOWNLOAD THREAD STARTED.");
    uint64_t time_st = photon::now;
    while (photon::now - time_st < delay_sec * 1000000) {
        photon::thread_usleep(200 * 1000);
        if (running != 1)
            break;
    }

    while (!dl_list.empty()) {
        if (running != 1) {
            LOG_WARN("image exited, background download exit...");
            break;
        }
        photon::thread_usleep(200 * 1000);

        BKDL::BkDownload *dl_item = dl_list.front();
        dl_list.pop_front();

        LOG_INFO("start downloading for dir `", dl_item->dir);

        if (!dl_item->lock_file()) {
            dl_list.push_back(dl_item);
            continue;
        }

        bool succ = dl_item->download(running);
        dl_item->unlock_file();

        if (running != 1) {
            LOG_WARN("image exited, background download exit...");
            delete dl_item;
            break;
        }

        if (!succ && dl_item->try_cnt > 0) {
            dl_list.push_back(dl_item);
            LOG_WARN("download failed, push back to download queue and retry", dl_item->dir);
            continue;
        }
        LOG_DEBUG("finish downloading or no retry any more: `, retry_cnt: `", dl_item->dir,
                 dl_item->try_cnt);
        delete dl_item;
    }

    if (!dl_list.empty()) {
        LOG_INFO("DOWNLOAD THREAD EXITED in advance, delete dl_list.");
        while (!dl_list.empty()) {
            BKDL::BkDownload *dl_item = dl_list.front();
            dl_list.pop_front();
            delete dl_item;
        }
    }
    LOG_DEBUG("BACKGROUND DOWNLOAD THREAD EXIT.");
}

} // namespace BKDL