#ifndef _INC_CTESTPACKET_H
#define _INC_CTESTPACKET_H

#include "../../../src/common/common.h"
#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <vector>

// The bytes of a packet a fake client sends, big endian as on the network (after the decryption).
class CTestPacket
{
    std::vector<byte> _vData;
    bool _fVariable;

public:
    // A packet of fixed length.
    explicit CTestPacket(byte bId) : _vData{bId}, _fVariable(false)
    {
    }
    // A packet of variable length: the length follows the ID, written by Get.
    static CTestPacket Variable(byte bId)
    {
        CTestPacket packet(bId);
        packet._fVariable = true;
        packet.Word(0);
        return packet;
    }

    CTestPacket& Byte(byte b)
    {
        _vData.push_back(b);
        return *this;
    }
    CTestPacket& Word(word w)
    {
        _vData.push_back(byte(w >> 8));
        _vData.push_back(byte(w));
        return *this;
    }
    CTestPacket& Dword(dword dw)
    {
        Word(word(dw >> 16));
        return Word(word(dw));
    }
    // An ASCII string in a field of that length, padded with zeros.
    CTestPacket& String(std::string_view sv, size_t uiFieldLength)
    {
        for (size_t i = 0; i < uiFieldLength; ++i)
            _vData.push_back((i < sv.size()) ? byte(sv[i]) : byte(0));
        return *this;
    }

    std::vector<byte> Get() const
    {
        std::vector<byte> vData(_vData);
        if (_fVariable)
        {
            vData[1] = byte(vData.size() >> 8);
            vData[2] = byte(vData.size());
        }
        return vData;
    }
};

// Reads a packet sent to a fake client.
class CTestPacketReader
{
    const std::vector<byte>& _vData;
    size_t _uiPos;

public:
    explicit CTestPacketReader(const std::vector<byte>& vData, size_t uiPos = 0) : _vData(vData), _uiPos(uiPos)
    {
    }

    byte Byte()
    {
        if (_uiPos >= _vData.size())
            throw std::out_of_range("CTestPacketReader: past the end of the packet");
        return _vData[_uiPos++];
    }
    word Word()
    {
        const word wHi = Byte();
        return word((wHi << 8) | Byte());
    }
    dword Dword()
    {
        const dword dwHi = Word();
        return (dwHi << 16) | Word();
    }
    CTestPacketReader& Skip(size_t uiBytes)
    {
        _uiPos += uiBytes;
        return *this;
    }
};

#endif // _INC_CTESTPACKET_H
