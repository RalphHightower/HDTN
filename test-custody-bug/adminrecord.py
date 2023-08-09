from scapy.packet import Packet, bind_layers, split_layers
from scapy.fields import BitEnumField, BitField, ConditionalField, StrLenField, ByteEnumField
from scapy.contrib.sdnv import SDNV2, SDNV2FieldLenField, SDNVUtil
from scapy.contrib.bp import BPBLOCK, BP
from scapy.compat import raw
import struct

class BpBlock(Packet):
    def guess_payload_class(self, payload):
        ul = self.underlayer
        if isinstance(ul, BP):
            bundle = ul
            if (bundle.ProcFlags & 2) == 2:
                return AdminRecord
        return Packet.guess_payload_class(self, payload)

    fields_desc = [
            ByteEnumField(name="Type", default=1, enum={1 : "payload"}),
            SDNV2("flags", 0),
            SDNV2("block_len", default=0)
            ]

    def post_build(self, p, pay):
        if self.block_len == 0 and pay:
            # Need to update
            p = (struct.pack('B', self.Type)
            + raw(SDNVUtil.encode(self.flags))
            + raw(SDNVUtil.encode(len(pay))))
        return p + pay



    def mysummary(self):
        return self.sprintf("BpBlock t:%Type% f:%flags% l:%block_len%")

class AdminRecord(Packet):
    fields_desc = [
        BitEnumField(
            name="Type",
            default=0b0000,
            size=4,
            enum={0b0001: "status report", 0b0010: "custody signal"},
        ),
        BitEnumField(
            name="flags", default=0b0000, size=4, enum={0b0000: "None", 0b0001: "for fragment"}
        ),
    ]

    def mysummary(self):
        return self.sprintf("AdminRecord t:%Type% f:%flags%")


def _is_for_fragment(pkt):
    if not pkt.underlayer or not 'AdminRecord' in pkt.underlayer:
        return False
    return (pkt.underlayer.flags & 0b0001) == 0b0001

class CustodySignal(Packet):
    fields_desc = [
        BitField(name="succeeded", default=0, size=1),
        BitEnumField(
            name="reason",
            default=0,
            size=7,
            enum={
                0x00: "no addl info",
                0x01: "reserved",
                0x02: "reserved",
                0x03: "extra recv",
                0x04: "depltd str",
                0x05: "dest ID bad",
                0x06: "no route",
                0x07: "no contact",
                0x08: "bad block",
            },
        ),
        ConditionalField(SDNV2("fragment_offset", 0), _is_for_fragment),
        ConditionalField(SDNV2("fragment_length", 0), _is_for_fragment),
        SDNV2("sig_time_sec", default=0),
        SDNV2("sig_time_nano", default=0),
        SDNV2("creat_time", default=0),
        SDNV2("creat_seq", default=0),
        SDNV2FieldLenField("src_eid_len", None, length_of="src_eid"),
        StrLenField(
            "src_eid",
            default=b"",
            length_from=(lambda pkt: pkt.src_eid_len)
        ),
    ]

    def mysummary(self):
        return self.sprintf("CS ok:%succeeded%:<%reason%> st:%sig_time_sec%.%sig_time_nano% ct:%creat_time%-%creat_seq% sl: %src_eid_len% s:%src_eid%")

split_layers(BP, BPBLOCK)
bind_layers(AdminRecord, CustodySignal, Type=2)
bind_layers(BP, BpBlock)
