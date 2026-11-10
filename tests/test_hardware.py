"""Host tests of packet validation, native power reads and DXE lifecycle."""
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class HardwareTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory()
        cls.out=Path(cls.temp.name)
        cls.cc=os.environ.get('CC','cc')
        cls.abi=Path(os.environ['PJB_BOOT_ABI_INCLUDE'])
        cls.edk=Path(os.environ.get('PJB_EDK2_ROOT') or ROOT/'edk2')
        subprocess.run([cls.cc,'-shared','-fPIC','-Wall','-Wextra','-Werror',
            str(ROOT/'Ums9620Pkg/Drivers/BoardServicesDxe/TouchPacket.c'),
            '-o',str(cls.out/'touch.so')],check=True)
        cls.lib=ctypes.CDLL(str(cls.out/'touch.so'))
        cls.parse=cls.lib.W202ParseTouch
        cls.parse.argtypes=[ctypes.c_void_p,ctypes.c_uint]+[ctypes.POINTER(ctypes.c_uint)]*3

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def packet(self,x=100,y=200,count=1):
        p=bytearray(b'\xff'*40+b'\0'*16)
        if count:p[:4]=x.to_bytes(2,'big')+y.to_bytes(2,'big')
        p[52]=count;p[55]=(-sum(p))&255
        return p

    def decode(self,p):
        x,y,down=[ctypes.c_uint(99) for _ in range(3)]
        b=(ctypes.c_ubyte*len(p)).from_buffer_copy(p)
        result=self.parse(b,len(p),ctypes.byref(x),ctypes.byref(y),ctypes.byref(down))
        return result,x.value,y.value,down.value

    def test_press_release_and_edges(self):
        self.assertEqual(self.decode(self.packet()),(1,100,200,1))
        self.assertEqual(self.decode(self.packet(1199,1919)),(1,1199,1919,1))
        self.assertEqual(self.decode(self.packet(count=0)),(0,99,99,0))
        p=self.packet();p[52]|=0xf0;p[55]=0;p[55]=(-sum(p))&255
        self.assertEqual(self.decode(p),(1,100,200,1))

    def test_invalid_frames_do_not_mutate_output(self):
        p=self.packet();p[0]^=1
        for bad in [p,bytearray(56),bytearray([255]*56),self.packet(1200,200),
                    self.packet(100,1920),self.packet(count=2),self.packet()[:55]]:
            self.assertEqual(self.decode(bad),(-1,99,99,99))

    def test_native_panel_corners_remain_unrotated(self):
        for x,y in [(0,0),(1199,1919),(457,1188)]:
            self.assertEqual(self.decode(self.packet(x,y)),(1,x,y,1))

    def test_real_dxe_lifecycle_and_read_only_power(self):
        for name in ['hardware_state','watchdog','power_off','touch_start','touch_upload','touch_resume','touch_irq','handoff_record','power_read','adc_read','charger_read','charger_control','keys','keys_probe','gop_refresh','emmc_read','emmc_gpt']:
            output=self.out/name
            subprocess.run([self.cc,'-std=c11','-ffunction-sections','-fdata-sections',
                '-Wl,--gc-sections','-fshort-wchar','-fno-builtin','-Werror',
                '-DEFIAPI=__attribute__((ms_abi))',
                '-I'+str(self.edk/'MdePkg/Include'),'-I'+str(self.edk/'MdePkg/Include/X64'),
                '-I'+str(self.edk/'MdeModulePkg/Include'),
                '-I'+str(self.edk/'EmbeddedPkg/Include'),
                '-I'+str(self.abi),
                str(ROOT/'tests'/f'{name}.c'),'-o',str(output)],check=True)
            subprocess.run([str(output)],check=True)

if __name__=='__main__':unittest.main()
