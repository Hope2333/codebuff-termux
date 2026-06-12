// Override os.cpus() before codebuff loads
import os from 'node:os';
const _orig = os.cpus.bind(os);
os.cpus = function patchedCpus() {
    try {
        return _orig();
    } catch(e) {
        console.error("[PRELOAD] os.cpus() failed, returning mock:", e.message);
        return [{model:'ARM',speed:0,times:{user:0,nice:0,sys:0,idle:0,irq:0}}];
    }
};
console.error("[PRELOAD] os.cpus() patched!");
