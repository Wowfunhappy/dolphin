// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// Based off of tachtig/twintig http://git.infradead.org/?p=users/segher/wii.git
// Copyright 2007,2008  Segher Boessenkool  <segher@kernel.crashing.org>
// Licensed under the terms of the GNU GPL, version 2
// http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt

#include "Core/HW/WiiSave.h"

#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mbedtls/aes.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha1.h>
#include <memory>
#include <string>
#include <vector>

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/ec.h"
#include "Common/File.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"

using Md5 = std::array<u8, 0x10>;

constexpr std::array<u8, 0x10> s_sd_initial_iv{{0x21, 0x67, 0x12, 0xE6, 0xAA, 0x1F, 0x68, 0x9F,
                                                0x95, 0xC5, 0xA2, 0x23, 0x24, 0xDC, 0x6A, 0x98}};
constexpr std::array<u8, 0x10> s_sd_key{{0xAB, 0x01, 0xB9, 0xD8, 0xE1, 0x62, 0x2B, 0x08, 0xAF, 0xBA,
                                         0xD8, 0x4D, 0xBF, 0xC2, 0xA5, 0x5D}};
constexpr Md5 s_md5_blanker{{0x0E, 0x65, 0x37, 0x81, 0x99, 0xBE, 0x45, 0x17, 0xAB, 0x06, 0xEC, 0x22,
                             0x45, 0x1A, 0x57, 0x93}};
constexpr u32 s_ng_id = 0x0403AC68;

bool WiiSave::Import(std::string filename)
{
  WiiSave save_file{std::move(filename)};
  return save_file.Import();
}

bool WiiSave::Export(u64 title_id, std::string export_path)
{
  WiiSave export_save{title_id, std::move(export_path)};
  return export_save.Export();
}

size_t WiiSave::ExportAll(std::string export_path)
{
  std::string title_folder = File::GetUserPath(D_WIIROOT_IDX) + "/title";
  std::vector<u64> titles;
  const u32 path_mask = 0x00010000;
  for (int i = 0; i < 8; ++i)
  {
    std::string folder = StringFromFormat("%s/%08x/", title_folder.c_str(), path_mask | i);
    File::FSTEntry fst_tmp = File::ScanDirectoryTree(folder, false);

    for (const File::FSTEntry& entry : fst_tmp.children)
    {
      if (entry.isDirectory)
      {
        u32 game_id;
        if (AsciiToHex(entry.virtualName, game_id))
        {
          std::string banner_path =
              StringFromFormat("%s%08x/data/banner.bin", folder.c_str(), game_id);
          if (File::Exists(banner_path))
          {
            u64 title_id = (((u64)path_mask | i) << 32) | game_id;
            titles.push_back(title_id);
          }
        }
      }
    }
  }
  size_t exported_save_count = 0;
  for (const u64& title : titles)
  {
    WiiSave export_save{title, export_path};
    if (export_save.Export())
      ++exported_save_count;
  }
  return exported_save_count;
}

WiiSave::WiiSave(std::string filename)
    : m_sd_iv{s_sd_initial_iv}, m_encrypted_save_path(std::move(filename)), m_valid{true}
{
  mbedtls_aes_setkey_dec(&m_aes_ctx, s_sd_key.data(), 128);
}

bool WiiSave::Import()
{
  ReadHDR();
  ReadBKHDR();
  ImportWiiSaveFiles();
  // TODO: check_sig()
  return m_valid;
}

WiiSave::WiiSave(u64 title_id, std::string export_path)
    : m_sd_iv{s_sd_initial_iv}, m_encrypted_save_path(std::move(export_path)), m_title_id{title_id}
{
  mbedtls_aes_setkey_enc(&m_aes_ctx, s_sd_key.data(), 128);

  if (getPaths(true))
    m_valid = true;
}

bool WiiSave::Export()
{
  WriteHDR();
  WriteBKHDR();
  ExportWiiSaveFiles();
  do_sig();
  return m_valid;
}

void WiiSave::ReadHDR()
{
  File::IOFile data_file(m_encrypted_save_path, "rb");
  if (!data_file)
  {
    ERROR_LOG(CONSOLE, "Cannot open %s", m_encrypted_save_path.c_str());
    m_valid = false;
    return;
  }
  if (!data_file.ReadBytes(&m_encrypted_header, HEADER_SZ))
  {
    ERROR_LOG(CONSOLE, "Failed to read header");
    m_valid = false;
    return;
  }
  data_file.Close();

  mbedtls_aes_crypt_cbc(&m_aes_ctx, MBEDTLS_AES_DECRYPT, HEADER_SZ, m_sd_iv.data(),
                        (const u8*)&m_encrypted_header, (u8*)&m_header);
  u32 banner_size = m_header.hdr.banner_size;
  if ((banner_size < FULL_BNR_MIN) || (banner_size > FULL_BNR_MAX) ||
      (((banner_size - BNR_SZ) % ICON_SZ) != 0))
  {
    ERROR_LOG(CONSOLE, "Not a Wii save or read failure for file header size %x", banner_size);
    m_valid = false;
    return;
  }
  m_title_id = m_header.hdr.save_game_title;

  Md5 md5_file = m_header.hdr.md5;
  m_header.hdr.md5 = s_md5_blanker;
  Md5 md5_calc;
  mbedtls_md5((u8*)&m_header, HEADER_SZ, md5_calc.data());
  if (md5_file == md5_calc)
  {
    ERROR_LOG(CONSOLE, "MD5 mismatch\n %016" PRIx64 "%016" PRIx64 " != %016" PRIx64 "%016" PRIx64,
              Common::swap64(md5_file.data()), Common::swap64(md5_file.data() + 8),
              Common::swap64(md5_calc.data()), Common::swap64(md5_calc.data() + 8));
    m_valid = false;
  }

  if (!getPaths())
  {
    m_valid = false;
    return;
  }
  std::string banner_file_path = m_wii_title_path + "/banner.bin";
  if (!File::Exists(banner_file_path) ||
      AskYesNoT("%s already exists. Consider making a backup of the current save files before "
                "overwriting.\nOverwrite now?",
                banner_file_path.c_str()))
  {
    INFO_LOG(CONSOLE, "Creating file %s", banner_file_path.c_str());
    File::IOFile banner_file(banner_file_path, "wb");
    banner_file.WriteBytes(m_header.banner, banner_size);
  }
  else
  {
    m_valid = false;
  }
}

void WiiSave::WriteHDR()
{
  if (!m_valid)
    return;
  memset(&m_header, 0, HEADER_SZ);

  std::string banner_file_path = m_wii_title_path + "/banner.bin";
  u32 banner_size = static_cast<u32>(File::GetSize(banner_file_path));
  m_header.hdr.banner_size = banner_size;

  m_header.hdr.save_game_title = m_title_id;
  m_header.hdr.md5 = s_md5_blanker;
  m_header.hdr.permissions = 0x3C;

  File::IOFile banner_file(banner_file_path, "rb");
  if (!banner_file.ReadBytes(m_header.banner, banner_size))
  {
    ERROR_LOG(CONSOLE, "Failed to read banner.bin");
    m_valid = false;
    return;
  }
  // remove nocopy flag
  m_header.banner[7] &= ~1;

  Md5 md5_calc;
  mbedtls_md5((u8*)&m_header, HEADER_SZ, md5_calc.data());
  m_header.hdr.md5 = std::move(md5_calc);

  mbedtls_aes_crypt_cbc(&m_aes_ctx, MBEDTLS_AES_ENCRYPT, HEADER_SZ, m_sd_iv.data(),
                        (const u8*)&m_header, (u8*)&m_encrypted_header);

  File::IOFile data_file(m_encrypted_save_path, "wb");
  if (!data_file.WriteBytes(&m_encrypted_header, HEADER_SZ))
  {
    ERROR_LOG(CONSOLE, "Failed to write header for %s", m_encrypted_save_path.c_str());
    m_valid = false;
  }
}

void WiiSave::ReadBKHDR()
{
  if (!m_valid)
    return;

  File::IOFile fpData_bin(m_encrypted_save_path, "rb");
  if (!fpData_bin)
  {
    ERROR_LOG(CONSOLE, "Cannot open %s", m_encrypted_save_path.c_str());
    m_valid = false;
    return;
  }
  fpData_bin.Seek(HEADER_SZ, SEEK_SET);
  if (!fpData_bin.ReadBytes(&m_bk_hdr, BK_SZ))
  {
    ERROR_LOG(CONSOLE, "Failed to read bk header");
    m_valid = false;
    return;
  }
  fpData_bin.Close();

  if (m_bk_hdr.size != BK_LISTED_SZ || m_bk_hdr.magic != BK_HDR_MAGIC)
  {
    ERROR_LOG(CONSOLE, "Invalid Size(%x) or Magic word (%x)", u32(m_bk_hdr.size),
              u32(m_bk_hdr.magic));
    m_valid = false;
    return;
  }

  if (m_bk_hdr.size_of_files + FULL_CERT_SZ != m_bk_hdr.total_size)
  {
    WARN_LOG(CONSOLE, "Size(%x) + cert(%x) does not equal totalsize(%x)",
             u32(m_bk_hdr.size_of_files), FULL_CERT_SZ, u32(m_bk_hdr.total_size));
  }
  if (m_title_id != m_bk_hdr.save_game_title)
  {
    WARN_LOG(CONSOLE,
             "Encrypted title (%" PRIx64 ") does not match unencrypted title (%" PRIx64 ")",
             m_title_id, u64(m_bk_hdr.save_game_title));
  }
}

void WiiSave::WriteBKHDR()
{
  if (!m_valid)
    return;
  u32 number_of_files = 0, size_of_files = 0;
  ScanForFiles(m_wii_title_path, m_files_list, &number_of_files, &size_of_files);
  memset(&m_bk_hdr, 0, BK_SZ);
  m_bk_hdr.size = BK_LISTED_SZ;
  m_bk_hdr.magic = BK_HDR_MAGIC;
  m_bk_hdr.ngid = s_ng_id;
  m_bk_hdr.number_of_files = number_of_files;
  m_bk_hdr.size_of_files = size_of_files;
  m_bk_hdr.total_size = size_of_files + FULL_CERT_SZ;
  m_bk_hdr.save_game_title = m_title_id;

  File::IOFile data_file(m_encrypted_save_path, "ab");
  if (!data_file.WriteBytes(&m_bk_hdr, BK_SZ))
  {
    ERROR_LOG(CONSOLE, "Failed to write bkhdr");
    m_valid = false;
  }
}

void WiiSave::ImportWiiSaveFiles()
{
  if (!m_valid)
    return;

  File::IOFile data_file(m_encrypted_save_path, "rb");
  if (!data_file)
  {
    ERROR_LOG(CONSOLE, "Cannot open %s", m_encrypted_save_path.c_str());
    m_valid = false;
    return;
  }

  data_file.Seek(HEADER_SZ + BK_SZ, SEEK_SET);

  FileHDR file_hdr_tmp;

  for (u32 i = 0; i < m_bk_hdr.number_of_files; ++i)
  {
    memset(&file_hdr_tmp, 0, FILE_HDR_SZ);
    m_iv.fill(0);

    if (!data_file.ReadBytes(&file_hdr_tmp, FILE_HDR_SZ))
    {
      ERROR_LOG(CONSOLE, "Failed to read header for file %d", i);
      m_valid = false;
    }

    if (file_hdr_tmp.magic != FILE_HDR_MAGIC)
    {
      ERROR_LOG(CONSOLE, "Bad File Header");
      break;
    }
    else
    {
      // Allows files in subfolders to be escaped properly (ex: "nocopy/data00")
      // Special characters in path components will be escaped such as /../
      std::string file_path = Common::EscapePath(file_hdr_tmp.name.data());

      std::string file_path_full = m_wii_title_path + '/' + file_path;
      File::CreateFullPath(file_path_full);
      const File::FileInfo file_info(file_path_full);
      if (file_hdr_tmp.type == 1)
      {
        u32 file_size_rounded = Common::AlignUp<u32>(file_hdr_tmp.size, BLOCK_SZ);
        std::vector<u8> file_data(file_size_rounded);
        std::vector<u8> file_data_enc(file_size_rounded);

        if (!data_file.ReadBytes(file_data_enc.data(), file_size_rounded))
        {
          ERROR_LOG(CONSOLE, "Failed to read data from file %d", i);
          m_valid = false;
          break;
        }

        m_iv = file_hdr_tmp.iv;
        mbedtls_aes_crypt_cbc(&m_aes_ctx, MBEDTLS_AES_DECRYPT, file_size_rounded, m_iv.data(),
                              static_cast<const u8*>(file_data_enc.data()), file_data.data());

        INFO_LOG(CONSOLE, "Creating file %s", file_path_full.c_str());

        File::IOFile raw_save_file(file_path_full, "wb");
        raw_save_file.WriteBytes(file_data.data(), file_hdr_tmp.size);
      }
      else if (file_hdr_tmp.type == 2)
      {
        if (!file_info.Exists())
        {
          if (!File::CreateDir(file_path_full))
            ERROR_LOG(CONSOLE, "Failed to create directory %s", file_path_full.c_str());
        }
        else if (!file_info.IsDirectory())
        {
          ERROR_LOG(CONSOLE,
                    "Failed to create directory %s because a file with the same name exists",
                    file_path_full.c_str());
        }
      }
    }
  }
}

void WiiSave::ExportWiiSaveFiles()
{
  if (!m_valid)
    return;

  for (u32 i = 0; i < m_bk_hdr.number_of_files; i++)
  {
    FileHDR file_hdr_tmp;
    memset(&file_hdr_tmp, 0, FILE_HDR_SZ);

    u32 file_size = 0;
    const File::FileInfo file_info(m_files_list[i]);
    if (file_info.IsDirectory())
    {
      file_hdr_tmp.type = 2;
    }
    else
    {
      file_size = static_cast<u32>(file_info.GetSize());
      file_hdr_tmp.type = 1;
    }

    u32 file_size_rounded = Common::AlignUp(file_size, BLOCK_SZ);
    file_hdr_tmp.magic = FILE_HDR_MAGIC;
    file_hdr_tmp.size = file_size;
    file_hdr_tmp.permissions = 0x3c;

    std::string name =
        Common::UnescapeFileName(m_files_list[i].substr(m_wii_title_path.length() + 1));

    if (name.length() > 0x44)
    {
      ERROR_LOG(CONSOLE, "\"%s\" is too long for the filename, max length is 0x44 + \\0",
                name.c_str());
      m_valid = false;
      return;
    }
    std::strncpy(file_hdr_tmp.name.data(), name.c_str(), file_hdr_tmp.name.size());

    {
      File::IOFile fpData_bin(m_encrypted_save_path, "ab");
      fpData_bin.WriteBytes(&file_hdr_tmp, FILE_HDR_SZ);
    }

    if (file_hdr_tmp.type == 1)
    {
      if (file_size == 0)
      {
        ERROR_LOG(CONSOLE, "%s is a 0 byte file", m_files_list[i].c_str());
        m_valid = false;
        return;
      }
      File::IOFile raw_save_file(m_files_list[i], "rb");
      if (!raw_save_file)
      {
        ERROR_LOG(CONSOLE, "%s failed to open", m_files_list[i].c_str());
        m_valid = false;
      }

      std::vector<u8> file_data(file_size_rounded);
      std::vector<u8> file_data_enc(file_size_rounded);

      if (!raw_save_file.ReadBytes(file_data.data(), file_size))
      {
        ERROR_LOG(CONSOLE, "Failed to read data from file: %s", m_files_list[i].c_str());
        m_valid = false;
      }

      mbedtls_aes_crypt_cbc(&m_aes_ctx, MBEDTLS_AES_ENCRYPT, file_size_rounded,
                            file_hdr_tmp.iv.data(), static_cast<const u8*>(file_data.data()),
                            file_data_enc.data());

      File::IOFile fpData_bin(m_encrypted_save_path, "ab");
      if (!fpData_bin.WriteBytes(file_data_enc.data(), file_size_rounded))
      {
        ERROR_LOG(CONSOLE, "Failed to write data to file: %s", m_encrypted_save_path.c_str());
      }
    }
  }
}

void WiiSave::do_sig()
{
  if (!m_valid)
    return;
  u8 sig[0x40];
  u8 ng_cert[0x180];
  u8 ap_cert[0x180];
  u8 hash[0x14];
  u8 ap_priv[30];
  u8 ap_sig[60];
  char signer[64];
  char name[64];
  u32 data_size;

  const u32 ng_key_id = 0x6AAB8C59;

  const u8 ng_priv[30] = {0,    0xAB, 0xEE, 0xC1, 0xDD, 0xB4, 0xA6, 0x16, 0x6B, 0x70,
                          0xFD, 0x7E, 0x56, 0x67, 0x70, 0x57, 0x55, 0x27, 0x38, 0xA3,
                          0x26, 0xC5, 0x46, 0x16, 0xF7, 0x62, 0xC9, 0xED, 0x73, 0xF2};

  const u8 ng_sig[0x3C] = {0,    0xD8, 0x81, 0x63, 0xB2, 0x00, 0x6B, 0x0B, 0x54, 0x82, 0x88, 0x63,
                           0x81, 0x1C, 0x00, 0x71, 0x12, 0xED, 0xB7, 0xFD, 0x21, 0xAB, 0x0E, 0x50,
                           0x0E, 0x1F, 0xBF, 0x78, 0xAD, 0x37, 0x00, 0x71, 0x8D, 0x82, 0x41, 0xEE,
                           0x45, 0x11, 0xC7, 0x3B, 0xAC, 0x08, 0xB6, 0x83, 0xDC, 0x05, 0xB8, 0xA8,
                           0x90, 0x1F, 0xA8, 0x2A, 0x0E, 0x4E, 0x76, 0xEF, 0x44, 0x72, 0x99, 0xF8};

  sprintf(signer, "Root-CA00000001-MS00000002");
  sprintf(name, "NG%08x", s_ng_id);
  make_ec_cert(ng_cert, ng_sig, signer, name, ng_priv, ng_key_id);

  memset(ap_priv, 0, sizeof ap_priv);
  ap_priv[10] = 1;

  memset(ap_sig, 81, sizeof ap_sig);  // temp

  sprintf(signer, "Root-CA00000001-MS00000002-NG%08x", s_ng_id);
  sprintf(name, "AP%08x%08x", 1, 2);
  make_ec_cert(ap_cert, ap_sig, signer, name, ap_priv, 0);

  mbedtls_sha1(ap_cert + 0x80, 0x100, hash);
  generate_ecdsa(ap_sig, ap_sig + 30, ng_priv, hash);
  make_ec_cert(ap_cert, ap_sig, signer, name, ap_priv, 0);

  data_size = m_bk_hdr.size_of_files + 0x80;

  File::IOFile data_file(m_encrypted_save_path, "rb");
  if (!data_file)
  {
    m_valid = false;
    return;
  }
  auto data = std::make_unique<u8[]>(data_size);

  data_file.Seek(0xf0c0, SEEK_SET);
  if (!data_file.ReadBytes(data.get(), data_size))
  {
    m_valid = false;
    return;
  }

  mbedtls_sha1(data.get(), data_size, hash);
  mbedtls_sha1(hash, 20, hash);

  data_file.Open(m_encrypted_save_path, "ab");
  if (!data_file)
  {
    m_valid = false;
    return;
  }
  generate_ecdsa(sig, sig + 30, ap_priv, hash);
  *(u32*)(sig + 60) = Common::swap32(0x2f536969);

  data_file.WriteArray(sig, sizeof(sig));
  data_file.WriteArray(ng_cert, sizeof(ng_cert));
  data_file.WriteArray(ap_cert, sizeof(ap_cert));

  m_valid = data_file.IsGood();
}

void WiiSave::make_ec_cert(u8* cert, const u8* sig, const char* signer, const char* name,
                           const u8* priv, const u32 key_id)
{
  memset(cert, 0, 0x180);
  *(u32*)cert = Common::swap32(0x10002);

  memcpy(cert + 4, sig, 60);
  strcpy((char*)cert + 0x80, signer);
  *(u32*)(cert + 0xc0) = Common::swap32(2);
  strcpy((char*)cert + 0xc4, name);
  *(u32*)(cert + 0x104) = Common::swap32(key_id);
  ec_priv_to_pub(priv, cert + 0x108);
}

bool WiiSave::getPaths(bool for_export)
{
  if (m_title_id)
  {
    // CONFIGURED because this whole class is only used from the GUI, not directly by games.
    m_wii_title_path = Common::GetTitleDataPath(m_title_id, Common::FROM_CONFIGURED_ROOT);
  }

  if (for_export)
  {
    char game_id[5];
    sprintf(game_id, "%c%c%c%c", (u8)(m_title_id >> 24) & 0xFF, (u8)(m_title_id >> 16) & 0xFF,
            (u8)(m_title_id >> 8) & 0xFF, (u8)m_title_id & 0xFF);

    if (!File::IsDirectory(m_wii_title_path))
    {
      m_valid = false;
      ERROR_LOG(CONSOLE, "No save folder found for title %s", game_id);
      return false;
    }

    if (!File::Exists(m_wii_title_path + "/banner.bin"))
    {
      m_valid = false;
      ERROR_LOG(CONSOLE, "No banner file found for title %s", game_id);
      return false;
    }
    m_encrypted_save_path += StringFromFormat("/private/wii/title/%s/data.bin", game_id);
    File::CreateFullPath(m_encrypted_save_path);
  }
  else
  {
    File::CreateFullPath(m_wii_title_path);
  }
  return true;
}

void WiiSave::ScanForFiles(const std::string& save_directory, std::vector<std::string>& file_list,
                           u32* num_files, u32* size_files)
{
  std::vector<std::string> directories;
  directories.push_back(save_directory);
  u32 num = 0;
  u32 size = 0;

  for (u32 i = 0; i < directories.size(); ++i)
  {
    if (i != 0)
    {
      // add dir to fst
      file_list.push_back(directories[i]);
    }

    File::FSTEntry fst_tmp = File::ScanDirectoryTree(directories[i], false);
    for (const File::FSTEntry& elem : fst_tmp.children)
    {
      if (elem.virtualName != "banner.bin")
      {
        num++;
        size += FILE_HDR_SZ;
        if (elem.isDirectory)
        {
          if (elem.virtualName == "nocopy" || elem.virtualName == "nomove")
          {
            NOTICE_LOG(CONSOLE,
                       "This save will likely require homebrew tools to copy to a real Wii.");
          }

          directories.push_back(elem.physicalName);
        }
        else
        {
          file_list.push_back(elem.physicalName);
          size += static_cast<u32>(Common::AlignUp(elem.size, BLOCK_SZ));
        }
      }
    }
  }

  *num_files = num;
  *size_files = size;
}

WiiSave::~WiiSave()
{
}