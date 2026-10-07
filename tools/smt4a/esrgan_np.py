"""Real-ESRGAN (RRDBNet) inference in pure NumPy, loading a PyTorch .pth without torch.

Usage: python esrgan_np.py model.pth in.png out.png
Works on RGBA: RGB through the network, alpha through the network as greyscale.
"""
import pickle, zipfile, sys, collections
import numpy as np
from PIL import Image

DT = {'FloatStorage': np.float32, 'HalfStorage': np.float16}


def load_pth(path):
    z = zipfile.ZipFile(path)
    prefix = z.namelist()[0].split('/')[0]

    class _Storage:
        def __init__(self, dtype, key):
            self.arr = np.frombuffer(z.read(f'{prefix}/data/{key}'), dtype=dtype)

    def rebuild(storage, offset, size, stride, *args):
        a = storage.arr
        if len(size) == 0:
            return a[offset].copy()
        item = a.itemsize
        return np.lib.stride_tricks.as_strided(
            a[offset:], shape=tuple(size), strides=tuple(s * item for s in stride)).astype(np.float32).copy()

    class U(pickle.Unpickler):
        def find_class(self, mod, name):
            if name == '_rebuild_tensor_v2':
                return rebuild
            if name.endswith('Storage'):
                return name
            if mod == 'collections' and name == 'OrderedDict':
                return collections.OrderedDict
            return super().find_class(mod, name)

        def persistent_load(self, pid):
            _, stype, key, _loc, _n = pid
            return _Storage(DT[stype if isinstance(stype, str) else stype], key)

    sd = U(z.open(f'{prefix}/data.pkl')).load()
    for k in ('params_ema', 'params'):
        if k in sd:
            sd = sd[k]
    return sd


def conv(x, w, b):
    """x: (C,H,W) float32, w: (O,C,3,3) -> (O,H,W), same padding."""
    C, H, W = x.shape
    O = w.shape[0]
    xp = np.pad(x, ((0, 0), (1, 1), (1, 1)))
    cols = np.lib.stride_tricks.sliding_window_view(xp, (3, 3), axis=(1, 2))  # C,H,W,3,3
    cols = cols.transpose(0, 3, 4, 1, 2).reshape(C * 9, H * W)
    out = w.reshape(O, C * 9) @ cols
    out += b[:, None]
    return out.reshape(O, H, W)


def lrelu(x):
    return np.where(x > 0, x, 0.2 * x)


class Net:
    def __init__(self, sd):
        self.p = {k: v for k, v in sd.items()}
        self.nb = len({k.split('.')[1] for k in sd if k.startswith('body.') and 'rdb' in k})

    def c(self, name, x):
        return conv(x, self.p[name + '.weight'], self.p[name + '.bias'])

    def rdb(self, pre, x):
        x1 = lrelu(self.c(pre + '.conv1', x))
        x2 = lrelu(self.c(pre + '.conv2', np.concatenate([x, x1])))
        x3 = lrelu(self.c(pre + '.conv3', np.concatenate([x, x1, x2])))
        x4 = lrelu(self.c(pre + '.conv4', np.concatenate([x, x1, x2, x3])))
        x5 = self.c(pre + '.conv5', np.concatenate([x, x1, x2, x3, x4]))
        return x5 * 0.2 + x

    def forward(self, img):  # img (3,H,W) 0..1
        feat = self.c('conv_first', img)
        b = feat
        for i in range(self.nb):
            o = b
            for r in ('rdb1', 'rdb2', 'rdb3'):
                o = self.rdb(f'body.{i}.{r}', o)
            b = o * 0.2 + b
        feat = feat + self.c('conv_body', b)
        feat = lrelu(self.c('conv_up1', feat.repeat(2, 1).repeat(2, 2)))
        feat = lrelu(self.c('conv_up2', feat.repeat(2, 1).repeat(2, 2)))
        return self.c('conv_last', lrelu(self.c('conv_hr', feat)))


def upscale_rgb(net, rgb, tile=48, pad=8):
    """rgb: (H,W,3) float 0..1 -> (4H,4W,3)."""
    H, W, _ = rgb.shape
    out = np.zeros((H * 4, W * 4, 3), np.float32)
    src = np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode='edge').transpose(2, 0, 1)
    for y in range(0, H, tile):
        for x in range(0, W, tile):
            th, tw = min(tile, H - y), min(tile, W - x)
            patch = src[:, y:y + th + 2 * pad, x:x + tw + 2 * pad]
            r = net.forward(patch)
            out[y * 4:(y + th) * 4, x * 4:(x + tw) * 4] = \
                r[:, pad * 4:(pad + th) * 4, pad * 4:(pad + tw) * 4].transpose(1, 2, 0)
    return np.clip(out, 0, 1)


def upscale_image(net, im):
    a = np.asarray(im.convert('RGBA')).astype(np.float32) / 255.0
    rgb = upscale_rgb(net, a[..., :3])
    alpha_src = a[..., 3]
    if alpha_src.min() < 1.0:
        al = upscale_rgb(net, np.repeat(alpha_src[..., None], 3, 2)).mean(2)
    else:
        al = np.ones(rgb.shape[:2], np.float32)
    out = np.dstack([rgb, al])
    return Image.fromarray((out * 255 + 0.5).astype(np.uint8), 'RGBA')


if __name__ == '__main__':
    net = Net(load_pth(sys.argv[1]))
    upscale_image(net, Image.open(sys.argv[2])).save(sys.argv[3])
