/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDDemuxCC.h"

#include "DVDDemuxUtils.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/contrib/cc_decoder708.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "guilib/LocalizeStrings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/ColorUtils.h"
#include "utils/StringUtils.h"

#include <algorithm>

namespace COLOR = KODI::UTILS::COLOR;

namespace
{
/*!
 * \brief Color formats for CC color conversion
 */
enum class ColorFormat
{
  RGB,
  ARGB
};

/*!
 * \brief Given the color code from the close caption decoder, returns the corresponding
 * color in rgb format (striping the alpha channel from argb)
 * 
 * \param[in] ccColor - a given color from the cc decoder
 * \param[in] format - the color format
 * \return the corresponding Color in rgb
 */
constexpr COLOR::Color CCColorConversion(const uint8_t ccColor, ColorFormat format)
{
  COLOR::Color color = COLOR::NONE;
  switch (ccColor)
  {
    case WHITE:
      color = COLOR::WHITE;
      break;
    case GREEN:
      color = COLOR::GREEN;
      break;
    case BLUE:
      color = COLOR::BLUE;
      break;
    case CYAN:
      color = COLOR::CYAN;
      break;
    case RED:
      color = COLOR::RED;
      break;
    case YELLOW:
      color = COLOR::YELLOW;
      break;
    case MAGENTA:
      color = COLOR::MAGENTA;
      break;
    case BLACK:
      color = COLOR::BLACK;
      break;
    default:
      break;
  }

  if (format == ColorFormat::RGB)
    return color & ~0xFF000000;

  return color;
}

/*!
 * \brief Given the current buffer cc text and cc style attributes, apply the modifiers
 * \param[in,out] ccText - the text to display in the caption
 * \param[in] ccAttributes - The attributes (italic, color, underline) for the text
 */
void ApplyStyleModifiers(std::string& ccText, const cc_attribute_t& ccAttributes)
{
  // Apply style modifiers to CC text
  if (ccAttributes.italic > 0)
  {
    ccText = StringUtils::Format("<i>{}</i>", ccText);
  }
  if (ccAttributes.underline > 0)
  {
    ccText = StringUtils::Format("<u>{}</u>", ccText);
  }
  if (ccAttributes.foreground != WHITE)
  {
    ccText = StringUtils::Format(
        "<font color=#{}>{}</u>",
        COLOR::ConvertToHexRGB(CCColorConversion(ccAttributes.foreground, ColorFormat::RGB)),
        ccText);
  }
}
} // namespace

class CBitstream
{
public:
  CBitstream(uint8_t *data, int bits)
  {
    m_data = data;
    m_offset = 0;
    m_len = bits;
    m_error = false;
  }
  unsigned int readBits(int num)
  {
    int r = 0;
    while (num > 0)
    {
      if (m_offset >= m_len)
      {
        m_error = true;
        return 0;
      }
      num--;
      if (m_data[m_offset / 8] & (1 << (7 - (m_offset & 7))))
        r |= 1 << num;
      m_offset++;
    }
    return r;
  }
  unsigned int readGolombUE(int maxbits = 32)
  {
    int lzb = -1;
    int bits = 0;
    for (int b = 0; !b; lzb++, bits++)
    {
      if (bits > maxbits)
        return 0;
      b = readBits(1);
    }
    return (1 << lzb) - 1 + readBits(lzb);
  }

private:
  uint8_t *m_data;
  int m_offset;
  int m_len;
  bool m_error;
};

class CCaptionBlock
{
  CCaptionBlock(const CCaptionBlock&) = delete;
  CCaptionBlock& operator=(const CCaptionBlock&) = delete;
public:
  explicit CCaptionBlock(int size)
  {
    m_data = (uint8_t*)malloc(size);
    m_size = size;
    m_pts = 0.0; //silence coverity uninitialized warning, is set elsewhere
  }
  virtual ~CCaptionBlock()
  {
    free(m_data);
  }
  double m_pts;
  uint8_t *m_data;
  int m_size;
};

bool reorder_sort (CCaptionBlock *lhs, CCaptionBlock *rhs)
{
  return (lhs->m_pts > rhs->m_pts);
}

CDVDDemuxCC::CDVDDemuxCC(AVCodecID codec) : m_codec(codec)
{
  m_hasData = false;
  m_curPts = 0.0;
}

CDVDDemuxCC::~CDVDDemuxCC()
{
  Dispose();
}

CDemuxStream* CDVDDemuxCC::GetStream(int iStreamId) const
{
  for (int i=0; i<GetNrOfStreams(); i++)
  {
    if (m_streams[i].uniqueId == iStreamId)
      return const_cast<CDemuxStreamSubtitle*>(&m_streams[i]);
  }
  return nullptr;
}

std::vector<CDemuxStream*> CDVDDemuxCC::GetStreams() const
{
  std::vector<CDemuxStream*> streams;

  int num = GetNrOfStreams();
  streams.reserve(num);
  for (int i = 0; i < num; ++i)
  {
    streams.push_back(const_cast<CDemuxStreamSubtitle*>(&m_streams[i]));
  }

  return streams;
}

int CDVDDemuxCC::GetNrOfStreams() const
{
  return m_streams.size();
}

DemuxPacket* CDVDDemuxCC::Read(DemuxPacket *pSrcPacket)
{
  DemuxPacket *pPacket = NULL;
  uint32_t startcode = 0xffffffff;
  int picType = 0;
  int p = 0;
  int len;

  if (!pSrcPacket)
  {
    pPacket = Decode();
    return pPacket;
  }
  if (pSrcPacket->pts == DVD_NOPTS_VALUE)
  {
    return pPacket;
  }

  while (!m_ccTempBuffer.empty())
  {
    m_ccReorderBuffer.push_back(m_ccTempBuffer.back());
    m_ccTempBuffer.pop_back();
  }

  while ((len = pSrcPacket->iSize - p) > 3)
  {
    if ((startcode & 0xffffff00) == 0x00000100)
    {
      if (m_codec == AV_CODEC_ID_MPEG2VIDEO)
      {
        int scode = startcode & 0xFF;
        if (scode == 0x00)
        {
          if (len > 4)
          {
            uint8_t *buf = pSrcPacket->pData + p;
            picType = (buf[1] & 0x38) >> 3;
          }
        }
        else if (scode == 0xb2) // user data
        {
          uint8_t *buf = pSrcPacket->pData + p;
          if (len >= 6 &&
            buf[0] == 'G' && buf[1] == 'A' && buf[2] == '9' && buf[3] == '4' &&
            buf[4] == 3 && (buf[5] & 0x40))
          {
            int cc_count = buf[5] & 0x1f;
            if (cc_count > 0 && len >= 7 + cc_count * 3)
            {
              CCaptionBlock *cc = new CCaptionBlock(cc_count * 3);
              memcpy(cc->m_data, buf + 7, cc_count * 3);
              cc->m_pts = pSrcPacket->pts;
              if (picType == 1 || picType == 2)
                m_ccTempBuffer.push_back(cc);
              else
                m_ccReorderBuffer.push_back(cc);
            }
          }
          else if (len >= 6 &&
                   buf[0] == 'C' && buf[1] == 'C' && buf[2] == 1)
          {
            int oddidx = (buf[4] & 0x80) ? 0 : 1;
            int cc_count = (buf[4] & 0x3e) >> 1;
            int extrafield = buf[4] & 0x01;
            if (extrafield)
              cc_count++;

            if (cc_count > 0 && len >= 5 + cc_count * 3 * 2)
            {
              CCaptionBlock *cc = new CCaptionBlock(cc_count * 3);
              uint8_t *src = buf + 5;
              uint8_t *dst = cc->m_data;

              for (int i = 0; i < cc_count; i++)
              {
                for (int j = 0; j < 2; j++)
                {
                  if (i == cc_count - 1 && extrafield && j == 1)
                    break;

                  if ((oddidx == j) && (src[0] == 0xFF))
                  {
                    dst[0] = 0x04;
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst += 3;
                  }
                  src += 3;
                }
              }
              cc->m_pts = pSrcPacket->pts;
              m_ccReorderBuffer.push_back(cc);
              picType = 1;
            }
          }
        }
      }
      else if (m_codec == AV_CODEC_ID_H264)
      {
        int scode = startcode & 0x9F;
        // slice data comes after SEI
        if (scode >= 1 && scode <= 5)
        {
          uint8_t *buf = pSrcPacket->pData + p;
          CBitstream bs(buf, len * 8);
          bs.readGolombUE();
          int sliceType = bs.readGolombUE();
          if (sliceType == 2 || sliceType == 7) // I slice
            picType = 1;
          else if (sliceType == 0 || sliceType == 5) // P slice
            picType = 2;
          if (picType == 0)
          {
            while (!m_ccTempBuffer.empty())
            {
              m_ccReorderBuffer.push_back(m_ccTempBuffer.back());
              m_ccTempBuffer.pop_back();
            }
          }
        }
        if (scode == 0x06) // SEI
        {
          uint8_t *buf = pSrcPacket->pData + p;
          if (len >= 12 &&
            buf[3] == 0 && buf[4] == 49 &&
            buf[5] == 'G' && buf[6] == 'A' && buf[7] == '9' && buf[8] == '4' && buf[9] == 3)
          {
            uint8_t *userdata = buf + 10;
            int cc_count = userdata[0] & 0x1f;
            if (len >= cc_count * 3 + 10)
            {
              CCaptionBlock *cc = new CCaptionBlock(cc_count * 3);
              memcpy(cc->m_data, userdata + 2, cc_count * 3);
              cc->m_pts = pSrcPacket->pts;
              m_ccTempBuffer.push_back(cc);
            }
          }
        }
      }
    }
    startcode = startcode << 8 | pSrcPacket->pData[p++];
  }

  if ((picType == 1 || picType == 2) && !m_ccReorderBuffer.empty())
  {
    if (!m_ccDecoder)
    {
      if (!OpenDecoder())
        return NULL;
    }
    std::sort(m_ccReorderBuffer.begin(), m_ccReorderBuffer.end(), reorder_sort);
    pPacket = Decode();
  }
  return pPacket;
}

void CDVDDemuxCC::Handler(int service, void *userdata)
{
  CDVDDemuxCC *ctx = static_cast<CDVDDemuxCC*>(userdata);

  unsigned int idx;

  // switch back from 608 fallback if we got 708
  if (ctx->m_ccDecoder->m_seen608 && ctx->m_ccDecoder->m_seen708)
  {
    for (idx = 0; idx < ctx->m_streamdata.size(); idx++)
    {
      if (ctx->m_streamdata[idx].service == 0)
        break;
    }
    if (idx < ctx->m_streamdata.size())
    {
      ctx->m_streamdata.erase(ctx->m_streamdata.begin() + idx);
      ctx->m_ccDecoder->m_seen608 = false;
    }
    if (service == 0)
      return;
  }

  for (idx = 0; idx < ctx->m_streamdata.size(); idx++)
  {
    if (ctx->m_streamdata[idx].service == service)
      break;
  }
  if (idx >= ctx->m_streamdata.size())
  {
    CDemuxStreamSubtitle stream;
    stream.source = STREAM_SOURCE_VIDEOMUX;
    stream.name = g_localizeStrings.Get(39206); // Closed Caption "CC"
    stream.language = "und";

    auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
    if (settings->GetBool(CSettings::SETTING_SUBTITLES_CAPTIONSIMPAIRED))
      stream.flags = FLAG_HEARING_IMPAIRED;

    stream.codec = AV_CODEC_ID_TEXT;
    stream.uniqueId = service;
    ctx->m_streams.push_back(std::move(stream));

    streamdata data;
    data.streamIdx = idx;
    data.service = service;
    ctx->m_streamdata.push_back(data);

    if (service == 0)
      ctx->m_ccDecoder->m_seen608 = true;
    else
      ctx->m_ccDecoder->m_seen708 = true;
  }

  ctx->m_streamdata[idx].pts = ctx->m_curPts;
  ctx->m_streamdata[idx].hasData = true;
  ctx->m_hasData = true;
}

bool CDVDDemuxCC::OpenDecoder()
{
  m_ccDecoder = std::make_unique<CDecoderCC708>();
  m_ccDecoder->Init(Handler, this);
  return true;
}

void CDVDDemuxCC::Dispose()
{
  m_streams.clear();
  m_streamdata.clear();
  m_ccDecoder.reset();

  while (!m_ccReorderBuffer.empty())
  {
    delete m_ccReorderBuffer.back();
    m_ccReorderBuffer.pop_back();
  }
  while (!m_ccTempBuffer.empty())
  {
    delete m_ccTempBuffer.back();
    m_ccTempBuffer.pop_back();
  }
}

DemuxPacket* CDVDDemuxCC::Decode()
{
  DemuxPacket *pPacket = NULL;

  while(!m_hasData && !m_ccReorderBuffer.empty())
  {
    CCaptionBlock *cc = m_ccReorderBuffer.back();
    m_ccReorderBuffer.pop_back();
    m_curPts = cc->m_pts;
    m_ccDecoder->Decode(cc->m_data, cc->m_size);
    delete cc;
  }

  if (m_hasData)
  {
    for (unsigned int i=0; i<m_streamdata.size(); i++)
    {
      if (m_streamdata[i].hasData)
      {
        int service = m_streamdata[i].service;

        std::string data;
        // CEA-608
        if (service == 0)
        {
          data = m_ccDecoder->m_cc608decoder->text;
          ApplyStyleModifiers(data, m_ccDecoder->m_cc608decoder->textattr);
        }
        // CEA-708
        else
        {
          data = m_ccDecoder->m_cc708decoders[service].text;
        }

        pPacket = CDVDDemuxUtils::AllocateDemuxPacket(data.size());
        pPacket->iSize = data.size();
        if (pPacket->iSize)
          memcpy(pPacket->pData, data.c_str(), pPacket->iSize);

        pPacket->iStreamId = service;
        pPacket->pts = m_streamdata[i].pts;
        pPacket->duration = 0;
        m_streamdata[i].hasData = false;
        break;
      }
      m_hasData = false;
    }
  }
  return pPacket;
}
