

#include <linux/blkdev.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_transport_fc.h>
#include <scsi/fc/fc_fs.h>
#include <linux/aer.h>

#include "lpfc_hw4.h"
#include "lpfc_hw.h"
#include "lpfc_sli.h"
#include "lpfc_sli4.h"
#include "lpfc_nl.h"
#include "lpfc_disc.h"
#include "lpfc_scsi.h"
#include "lpfc.h"
#include "lpfc_crtn.h"
#include "lpfc_logmsg.h"
#include "lpfc_compat.h"
#include "lpfc_debugfs.h"
#include "lpfc_vport.h"

/* There are only four IOCB completion types. */
typedef enum _lpfc_iocb_type {
	LPFC_UNKNOWN_IOCB,
	LPFC_UNSOL_IOCB,
	LPFC_SOL_IOCB,
	LPFC_ABORT_IOCB
} lpfc_iocb_type;


/* Provide function prototypes local to this module. */
static int lpfc_sli_issue_mbox_s4(struct lpfc_hba *, LPFC_MBOXQ_t *,
				  uint32_t);
static int lpfc_sli4_read_rev(struct lpfc_hba *, LPFC_MBOXQ_t *,
			      uint8_t *, uint32_t *);
static struct lpfc_iocbq *lpfc_sli4_els_wcqe_to_rspiocbq(struct lpfc_hba *,
							 struct lpfc_iocbq *);
static void lpfc_sli4_send_seq_to_ulp(struct lpfc_vport *,
				      struct hbq_dmabuf *);
static IOCB_t *
lpfc_get_iocb_from_iocbq(struct lpfc_iocbq *iocbq)
{
	return &iocbq->iocb;
}

static uint32_t
lpfc_sli4_wq_put(struct lpfc_queue *q, union lpfc_wqe *wqe)
{
	union lpfc_wqe *temp_wqe = q->qe[q->host_index].wqe;
	struct lpfc_register doorbell;
	uint32_t host_index;

	/* If the host has not yet processed the next entry then we are done */
	if (((q->host_index + 1) % q->entry_count) == q->hba_index)
		return -ENOMEM;
	/* set consumption flag every once in a while */
	if (!((q->host_index + 1) % LPFC_RELEASE_NOTIFICATION_INTERVAL))
		bf_set(lpfc_wqe_gen_wqec, &wqe->generic, 1);

	lpfc_sli_pcimem_bcopy(wqe, temp_wqe, q->entry_size);

	/* Update the host index before invoking device */
	host_index = q->host_index;
	q->host_index = ((q->host_index + 1) % q->entry_count);

	/* Ring Doorbell */
	doorbell.word0 = 0;
	bf_set(lpfc_wq_doorbell_num_posted, &doorbell, 1);
	bf_set(lpfc_wq_doorbell_index, &doorbell, host_index);
	bf_set(lpfc_wq_doorbell_id, &doorbell, q->queue_id);
	writel(doorbell.word0, q->phba->sli4_hba.WQDBregaddr);
	readl(q->phba->sli4_hba.WQDBregaddr); /* Flush */

	return 0;
}

static uint32_t
lpfc_sli4_wq_release(struct lpfc_queue *q, uint32_t index)
{
	uint32_t released = 0;

	if (q->hba_index == index)
		return 0;
	do {
		q->hba_index = ((q->hba_index + 1) % q->entry_count);
		released++;
	} while (q->hba_index != index);
	return released;
}

static uint32_t
lpfc_sli4_mq_put(struct lpfc_queue *q, struct lpfc_mqe *mqe)
{
	struct lpfc_mqe *temp_mqe = q->qe[q->host_index].mqe;
	struct lpfc_register doorbell;
	uint32_t host_index;

	/* If the host has not yet processed the next entry then we are done */
	if (((q->host_index + 1) % q->entry_count) == q->hba_index)
		return -ENOMEM;
	lpfc_sli_pcimem_bcopy(mqe, temp_mqe, q->entry_size);
	/* Save off the mailbox pointer for completion */
	q->phba->mbox = (MAILBOX_t *)temp_mqe;

	/* Update the host index before invoking device */
	host_index = q->host_index;
	q->host_index = ((q->host_index + 1) % q->entry_count);

	/* Ring Doorbell */
	doorbell.word0 = 0;
	bf_set(lpfc_mq_doorbell_num_posted, &doorbell, 1);
	bf_set(lpfc_mq_doorbell_id, &doorbell, q->queue_id);
	writel(doorbell.word0, q->phba->sli4_hba.MQDBregaddr);
	readl(q->phba->sli4_hba.MQDBregaddr); /* Flush */
	return 0;
}

static uint32_t
lpfc_sli4_mq_release(struct lpfc_queue *q)
{
	/* Clear the mailbox pointer for completion */
	q->phba->mbox = NULL;
	q->hba_index = ((q->hba_index + 1) % q->entry_count);
	return 1;
}

static struct lpfc_eqe *
lpfc_sli4_eq_get(struct lpfc_queue *q)
{
	struct lpfc_eqe *eqe = q->qe[q->hba_index].eqe;

	/* If the next EQE is not valid then we are done */
	if (!bf_get_le32(lpfc_eqe_valid, eqe))
		return NULL;
	/* If the host has not yet processed the next entry then we are done */
	if (((q->hba_index + 1) % q->entry_count) == q->host_index)
		return NULL;

	q->hba_index = ((q->hba_index + 1) % q->entry_count);
	return eqe;
}

uint32_t
lpfc_sli4_eq_release(struct lpfc_queue *q, bool arm)
{
	uint32_t released = 0;
	struct lpfc_eqe *temp_eqe;
	struct lpfc_register doorbell;

	/* while there are valid entries */
	while (q->hba_index != q->host_index) {
		temp_eqe = q->qe[q->host_index].eqe;
		bf_set_le32(lpfc_eqe_valid, temp_eqe, 0);
		released++;
		q->host_index = ((q->host_index + 1) % q->entry_count);
	}
	if (unlikely(released == 0 && !arm))
		return 0;

	/* ring doorbell for number popped */
	doorbell.word0 = 0;
	if (arm) {
		bf_set(lpfc_eqcq_doorbell_arm, &doorbell, 1);
		bf_set(lpfc_eqcq_doorbell_eqci, &doorbell, 1);
	}
	bf_set(lpfc_eqcq_doorbell_num_released, &doorbell, released);
	bf_set(lpfc_eqcq_doorbell_qt, &doorbell, LPFC_QUEUE_TYPE_EVENT);
	bf_set(lpfc_eqcq_doorbell_eqid, &doorbell, q->queue_id);
	writel(doorbell.word0, q->phba->sli4_hba.EQCQDBregaddr);
	/* PCI read to flush PCI pipeline on re-arming for INTx mode */
	if ((q->phba->intr_type == INTx) && (arm == LPFC_QUEUE_REARM))
		readl(q->phba->sli4_hba.EQCQDBregaddr);
	return released;
}

static struct lpfc_cqe *
lpfc_sli4_cq_get(struct lpfc_queue *q)
{
	struct lpfc_cqe *cqe;

	/* If the next CQE is not valid then we are done */
	if (!bf_get_le32(lpfc_cqe_valid, q->qe[q->hba_index].cqe))
		return NULL;
	/* If the host has not yet processed the next entry then we are done */
	if (((q->hba_index + 1) % q->entry_count) == q->host_index)
		return NULL;

	cqe = q->qe[q->hba_index].cqe;
	q->hba_index = ((q->hba_index + 1) % q->entry_count);
	return cqe;
}

uint32_t
lpfc_sli4_cq_release(struct lpfc_queue *q, bool arm)
{
	uint32_t released = 0;
	struct lpfc_cqe *temp_qe;
	struct lpfc_register doorbell;

	/* while there are valid entries */
	while (q->hba_index != q->host_index) {
		temp_qe = q->qe[q->host_index].cqe;
		bf_set_le32(lpfc_cqe_valid, temp_qe, 0);
		released++;
		q->host_index = ((q->host_index + 1) % q->entry_count);
	}
	if (unlikely(released == 0 && !arm))
		return 0;

	/* ring doorbell for number popped */
	doorbell.word0 = 0;
	if (arm)
		bf_set(lpfc_eqcq_doorbell_arm, &doorbell, 1);
	bf_set(lpfc_eqcq_doorbell_num_released, &doorbell, released);
	bf_set(lpfc_eqcq_doorbell_qt, &doorbell, LPFC_QUEUE_TYPE_COMPLETION);
	bf_set(lpfc_eqcq_doorbell_cqid, &doorbell, q->queue_id);
	writel(doorbell.word0, q->phba->sli4_hba.EQCQDBregaddr);
	return released;
}

static int
lpfc_sli4_rq_put(struct lpfc_queue *hq, struct lpfc_queue *dq,
		 struct lpfc_rqe *hrqe, struct lpfc_rqe *drqe)
{
	struct lpfc_rqe *temp_hrqe = hq->qe[hq->host_index].rqe;
	struct lpfc_rqe *temp_drqe = dq->qe[dq->host_index].rqe;
	struct lpfc_register doorbell;
	int put_index = hq->host_index;

	if (hq->type != LPFC_HRQ || dq->type != LPFC_DRQ)
		return -EINVAL;
	if (hq->host_index != dq->host_index)
		return -EINVAL;
	/* If the host has not yet processed the next entry then we are done */
	if (((hq->host_index + 1) % hq->entry_count) == hq->hba_index)
		return -EBUSY;
	lpfc_sli_pcimem_bcopy(hrqe, temp_hrqe, hq->entry_size);
	lpfc_sli_pcimem_bcopy(drqe, temp_drqe, dq->entry_size);

	/* Update the host index to point to the next slot */
	hq->host_index = ((hq->host_index + 1) % hq->entry_count);
	dq->host_index = ((dq->host_index + 1) % dq->entry_count);

	/* Ring The Header Receive Queue Doorbell */
	if (!(hq->host_index % LPFC_RQ_POST_BATCH)) {
		doorbell.word0 = 0;
		bf_set(lpfc_rq_doorbell_num_posted, &doorbell,
		       LPFC_RQ_POST_BATCH);
		bf_set(lpfc_rq_doorbell_id, &doorbell, hq->queue_id);
		writel(doorbell.word0, hq->phba->sli4_hba.RQDBregaddr);
	}
	return put_index;
}

static uint32_t
lpfc_sli4_rq_release(struct lpfc_queue *hq, struct lpfc_queue *dq)
{
	if ((hq->type != LPFC_HRQ) || (dq->type != LPFC_DRQ))
		return 0;
	hq->hba_index = ((hq->hba_index + 1) % hq->entry_count);
	dq->hba_index = ((dq->hba_index + 1) % dq->entry_count);
	return 1;
}

static inline IOCB_t *
lpfc_cmd_iocb(struct lpfc_hba *phba, struct lpfc_sli_ring *pring)
{
	return (IOCB_t *) (((char *) pring->cmdringaddr) +
			   pring->cmdidx * phba->iocb_cmd_size);
}

static inline IOCB_t *
lpfc_resp_iocb(struct lpfc_hba *phba, struct lpfc_sli_ring *pring)
{
	return (IOCB_t *) (((char *) pring->rspringaddr) +
			   pring->rspidx * phba->iocb_rsp_size);
}

static struct lpfc_iocbq *
__lpfc_sli_get_iocbq(struct lpfc_hba *phba)
{
	struct list_head *lpfc_iocb_list = &phba->lpfc_iocb_list;
	struct lpfc_iocbq * iocbq = NULL;

	list_remove_head(lpfc_iocb_list, iocbq, struct lpfc_iocbq, list);
	return iocbq;
}

static struct lpfc_sglq *
__lpfc_clear_active_sglq(struct lpfc_hba *phba, uint16_t xritag)
{
	uint16_t adj_xri;
	struct lpfc_sglq *sglq;
	adj_xri = xritag - phba->sli4_hba.max_cfg_param.xri_base;
	if (adj_xri > phba->sli4_hba.max_cfg_param.max_xri)
		return NULL;
	sglq = phba->sli4_hba.lpfc_sglq_active_list[adj_xri];
	phba->sli4_hba.lpfc_sglq_active_list[adj_xri] = NULL;
	return sglq;
}

struct lpfc_sglq *
__lpfc_get_active_sglq(struct lpfc_hba *phba, uint16_t xritag)
{
	uint16_t adj_xri;
	struct lpfc_sglq *sglq;
	adj_xri = xritag - phba->sli4_hba.max_cfg_param.xri_base;
	if (adj_xri > phba->sli4_hba.max_cfg_param.max_xri)
		return NULL;
	sglq =  phba->sli4_hba.lpfc_sglq_active_list[adj_xri];
	return sglq;
}

static struct lpfc_sglq *
__lpfc_sli_get_sglq(struct lpfc_hba *phba)
{
	struct list_head *lpfc_sgl_list = &phba->sli4_hba.lpfc_sgl_list;
	struct lpfc_sglq *sglq = NULL;
	uint16_t adj_xri;
	list_remove_head(lpfc_sgl_list, sglq, struct lpfc_sglq, list);
	if (!sglq)
		return NULL;
	adj_xri = sglq->sli4_xritag - phba->sli4_hba.max_cfg_param.xri_base;
	phba->sli4_hba.lpfc_sglq_active_list[adj_xri] = sglq;
	sglq->state = SGL_ALLOCATED;
	return sglq;
}

struct lpfc_iocbq *
lpfc_sli_get_iocbq(struct lpfc_hba *phba)
{
	struct lpfc_iocbq * iocbq = NULL;
	unsigned long iflags;

	spin_lock_irqsave(&phba->hbalock, iflags);
	iocbq = __lpfc_sli_get_iocbq(phba);
	spin_unlock_irqrestore(&phba->hbalock, iflags);
	return iocbq;
}

static void
__lpfc_sli_release_iocbq_s4(struct lpfc_hba *phba, struct lpfc_iocbq *iocbq)
{
	struct lpfc_sglq *sglq;
	size_t start_clean = offsetof(struct lpfc_iocbq, iocb);
	unsigned long iflag;

	if (iocbq->sli4_xritag == NO_XRI)
		sglq = NULL;
	else
		sglq = __lpfc_clear_active_sglq(phba, iocbq->sli4_xritag);
	if (sglq)  {
		if ((iocbq->iocb_flag & LPFC_EXCHANGE_BUSY) &&
			(sglq->state != SGL_XRI_ABORTED)) {
			spin_lock_irqsave(&phba->sli4_hba.abts_sgl_list_lock,
					iflag);
			list_add(&sglq->list,
				&phba->sli4_hba.lpfc_abts_els_sgl_list);
			spin_unlock_irqrestore(
				&phba->sli4_hba.abts_sgl_list_lock, iflag);
		} else {
			sglq->state = SGL_FREED;
			list_add(&sglq->list, &phba->sli4_hba.lpfc_sgl_list);
		}
	}


	/*
	 * Clean all volatile data fields, preserve iotag and node struct.
	 */
	memset((char *)iocbq + start_clean, 0, sizeof(*iocbq) - start_clean);
	iocbq->sli4_xritag = NO_XRI;
	list_add_tail(&iocbq->list, &phba->lpfc_iocb_list);
}

static void
__lpfc_sli_release_iocbq_s3(struct lpfc_hba *phba, struct lpfc_iocbq *iocbq)
{
	size_t start_clean = offsetof(struct lpfc_iocbq, iocb);

	/*
	 * Clean all volatile data fields, preserve iotag and node struct.
	 */
	memset((char*)iocbq + start_clean, 0, sizeof(*iocbq) - start_clean);
	iocbq->sli4_xritag = NO_XRI;
	list_add_tail(&iocbq->list, &phba->lpfc_iocb_list);
}

static void
__lpfc_sli_release_iocbq(struct lpfc_hba *phba, struct lpfc_iocbq *iocbq)
{
	phba->__lpfc_sli_release_iocbq(phba, iocbq);
}

void
lpfc_sli_release_iocbq(struct lpfc_hba *phba, struct lpfc_iocbq *iocbq)
{
	unsigned long iflags;

	/*
	 * Clean all volatile data fields, preserve iotag and node struct.
	 */
	spin_lock_irqsave(&phba->hbalock, iflags);
	__lpfc_sli_release_iocbq(phba, iocbq);
	spin_unlock_irqrestore(&phba->hbalock, iflags);
}

void
lpfc_sli_cancel_iocbs(struct lpfc_hba *phba, struct list_head *iocblist,
		      uint32_t ulpstatus, uint32_t ulpWord4)
{
	struct lpfc_iocbq *piocb;

	while (!list_empty(iocblist)) {
		list_remove_head(iocblist, piocb, struct lpfc_iocbq, list);

		if (!piocb->iocb_cmpl)
			lpfc_sli_release_iocbq(phba, piocb);
		else {
			piocb->iocb.ulpStatus = ulpstatus;
			piocb->iocb.un.ulpWord[4] = ulpWord4;
			(piocb->iocb_cmpl) (phba, piocb, piocb);
		}
	}
	return;
}

static lpfc_iocb_type
lpfc_sli_iocb_cmd_type(uint8_t iocb_cmnd)
{
	lpfc_iocb_type type = LPFC_UNKNOWN_IOCB;

	if (iocb_cmnd > CMD_MAX_IOCB_CMD)
		return 0;

	switch (iocb_cmnd) {
	case CMD_XMIT_SEQUENCE_CR:
	case CMD_XMIT_SEQUENCE_CX:
	case CMD_XMIT_BCAST_CN:
	case CMD_XMIT_BCAST_CX:
	case CMD_ELS_REQUEST_CR:
	case CMD_ELS_REQUEST_CX:
	case CMD_CREATE_XRI_CR:
	case CMD_CREATE_XRI_CX:
	case CMD_GET_RPI_CN:
	case CMD_XMIT_ELS_RSP_CX:
	case CMD_GET_RPI_CR:
	case CMD_FCP_IWRITE_CR:
	case CMD_FCP_IWRITE_CX:
	case CMD_FCP_IREAD_CR:
	case CMD_FCP_IREAD_CX:
	case CMD_FCP_ICMND_CR:
	case CMD_FCP_ICMND_CX:
	case CMD_FCP_TSEND_CX:
	case CMD_FCP_TRSP_CX:
	case CMD_FCP_TRECEIVE_CX:
	case CMD_FCP_AUTO_TRSP_CX:
	case CMD_ADAPTER_MSG:
	case CMD_ADAPTER_DUMP:
	case CMD_XMIT_SEQUENCE64_CR:
	case CMD_XMIT_SEQUENCE64_CX:
	case CMD_XMIT_BCAST64_CN:
	case CMD_XMIT_BCAST64_CX:
	case CMD_ELS_REQUEST64_CR:
	case CMD_ELS_REQUEST64_CX:
	case CMD_FCP_IWRITE64_CR:
	case CMD_FCP_IWRITE64_CX:
	case CMD_FCP_IREAD64_CR:
	case CMD_FCP_IREAD64_CX:
	case CMD_FCP_ICMND64_CR:
	case CMD_FCP_ICMND64_CX:
	case CMD_FCP_TSEND64_CX:
	case CMD_FCP_TRSP64_CX:
	case CMD_FCP_TRECEIVE64_CX:
	case CMD_GEN_REQUEST64_CR:
	case CMD_GEN_REQUEST64_CX:
	case CMD_XMIT_ELS_RSP64_CX:
	case DSSCMD_IWRITE64_CR:
	case DSSCMD_IWRITE64_CX:
	case DSSCMD_IREAD64_CR:
	case DSSCMD_IREAD64_CX:
		type = LPFC_SOL_IOCB;
		break;
	case CMD_ABORT_XRI_CN:
	case CMD_ABORT_XRI_CX:
	case CMD_CLOSE_XRI_CN:
	case CMD_CLOSE_XRI_CX:
	case CMD_XRI_ABORTED_CX:
	case CMD_ABORT_MXRI64_CN:
	case CMD_XMIT_BLS_RSP64_CX:
		type = LPFC_ABORT_IOCB;
		break;
	case CMD_RCV_SEQUENCE_CX:
	case CMD_RCV_ELS_REQ_CX:
	case CMD_RCV_SEQUENCE64_CX:
	case CMD_RCV_ELS_REQ64_CX:
	case CMD_ASYNC_STATUS:
	case CMD_IOCB_RCV_SEQ64_CX:
	case CMD_IOCB_RCV_ELS64_CX:
	case CMD_IOCB_RCV_CONT64_CX:
	case CMD_IOCB_RET_XRI64_CX:
		type = LPFC_UNSOL_IOCB;
		break;
	case CMD_IOCB_XMIT_MSEQ64_CR:
	case CMD_IOCB_XMIT_MSEQ64_CX:
	case CMD_IOCB_RCV_SEQ_LIST64_CX:
	case CMD_IOCB_RCV_ELS_LIST64_CX:
	case CMD_IOCB_CLOSE_EXTENDED_CN:
	case CMD_IOCB_ABORT_EXTENDED_CN:
	case CMD_IOCB_RET_HBQE64_CN:
	case CMD_IOCB_FCP_IBIDIR64_CR:
	case CMD_IOCB_FCP_IBIDIR64_CX:
	case CMD_IOCB_FCP_ITASKMGT64_CX:
	case CMD_IOCB_LOGENTRY_CN:
	case CMD_IOCB_LOGENTRY_ASYNC_CN:
		printk("%s - Unhandled SLI-3 Command x%x\n",
				__func__, iocb_cmnd);
		type = LPFC_UNKNOWN_IOCB;
		break;
	default:
		type = LPFC_UNKNOWN_IOCB;
		break;
	}

	return type;
}

static int
lpfc_sli_ring_map(struct lpfc_hba *phba)
{
	struct lpfc_sli *psli = &phba->sli;
	LPFC_MBOXQ_t *pmb;
	MAILBOX_t *pmbox;
	int i, rc, ret = 0;

	pmb = (LPFC_MBOXQ_t *) mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!pmb)
		return -ENOMEM;
	pmbox = &pmb->u.mb;
	phba->link_state = LPFC_INIT_MBX_CMDS;
	for (i = 0; i < psli->num_rings; i++) {
		lpfc_config_ring(phba, i, pmb);
		rc = lpfc_sli_issue_mbox(phba, pmb, MBX_POLL);
		if (rc != MBX_SUCCESS) {
			lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
					"0446 Adapter failed to init (%d), "
					"mbxCmd x%x CFG_RING, mbxStatus x%x, "
					"ring %d\n",
					rc, pmbox->mbxCommand,
					pmbox->mbxStatus, i);
			phba->link_state = LPFC_HBA_ERROR;
			ret = -ENXIO;
			break;
		}
	}
	mempool_free(pmb, phba->mbox_mem_pool);
	return ret;
}

static int
lpfc_sli_ringtxcmpl_put(struct lpfc_hba *phba, struct lpfc_sli_ring *pring,
			struct lpfc_iocbq *piocb)
{
	list_add_tail(&piocb->list, &pring->txcmplq);
	pring->txcmplq_cnt++;
	if ((unlikely(pring->ringno == LPFC_ELS_RING)) &&
	   (piocb->iocb.ulpCommand != CMD_ABORT_XRI_CN) &&
	   (piocb->iocb.ulpCommand != CMD_CLOSE_XRI_CN)) {
		if (!piocb->vport)
			BUG();
		else
			mod_timer(&piocb->vport->els_tmofunc,
				  jiffies + HZ * (phba->fc_ratov << 1));
	}


	return 0;
}

static struct lpfc_iocbq *
lpfc_sli_ringtx_get(struct lpfc_hba *phba, struct lpfc_sli_ring *pring)
{
	struct lpfc_iocbq *cmd_iocb;

	list_remove_head((&pring->txq), cmd_iocb, struct lpfc_iocbq, list);
	if (cmd_iocb != NULL)
		pring->txq_cnt--;
	return cmd_iocb;
}

static IOCB_t *
lpfc_sli_next_iocb_slot (struct lpfc_hba *phba, struct lpfc_sli_ring *pring)
{
	struct lpfc_pgp *pgp = &phba->port_gp[pring->ringno];
	uint32_t  max_cmd_idx = pring->numCiocb;
	if ((pring->next_cmdidx == pring->cmdidx) &&
	   (++pring->next_cmdidx >= max_cmd_idx))
		pring->next_cmdidx = 0;

	if (unlikely(pring->local_getidx == pring->next_cmdidx)) {

		pring->local_getidx = le32_to_cpu(pgp->cmdGetInx);

		if (unlikely(pring->local_getidx >= max_cmd_idx)) {
			lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
					"0315 Ring %d issue: portCmdGet %d "
					"is bigger than cmd ring %d\n",
					pring->ringno,
					pring->local_getidx, max_cmd_idx);

			phba->link_state = LPFC_HBA_ERROR;
			/*
			 * All error attention handlers are posted to
			 * worker thread
			 */
			phba->work_ha |= HA_ERATT;
			phba->work_hs = HS_FFER3;

			lpfc_worker_wake_up(phba);

			return NULL;
		}

		if (pring->local_getidx == pring->next_cmdidx)
			return NULL;
	}

	return lpfc_cmd_iocb(phba, pring);
}

uint16_t
lpfc_sli_next_iotag(struct lpfc_hba *phba, struct lpfc_iocbq *iocbq)
{
	struct lpfc_iocbq **new_arr;
	struct lpfc_iocbq **old_arr;
	size_t new_len;
	struct lpfc_sli *psli = &phba->sli;
	uint16_t iotag;

	spin_lock_irq(&phba->hbalock);
	iotag = psli->last_iotag;
	if(++iotag < psli->iocbq_lookup_len) {
		psli->last_iotag = iotag;
		psli->iocbq_lookup[iotag] = iocbq;
		spin_unlock_irq(&phba->hbalock);
		iocbq->iotag = iotag;
		return iotag;
	} else if (psli->iocbq_lookup_len < (0xffff
					   - LPFC_IOCBQ_LOOKUP_INCREMENT)) {
		new_len = psli->iocbq_lookup_len + LPFC_IOCBQ_LOOKUP_INCREMENT;
		spin_unlock_irq(&phba->hbalock);
		new_arr = kzalloc(new_len * sizeof (struct lpfc_iocbq *),
				  GFP_KERNEL);
		if (new_arr) {
			spin_lock_irq(&phba->hbalock);
			old_arr = psli->iocbq_lookup;
			if (new_len <= psli->iocbq_lookup_len) {
				/* highly unprobable case */
				kfree(new_arr);
				iotag = psli->last_iotag;
				if(++iotag < psli->iocbq_lookup_len) {
					psli->last_iotag = iotag;
					psli->iocbq_lookup[iotag] = iocbq;
					spin_unlock_irq(&phba->hbalock);
					iocbq->iotag = iotag;
					return iotag;
				}
				spin_unlock_irq(&phba->hbalock);
				return 0;
			}
			if (psli->iocbq_lookup)
				memcpy(new_arr, old_arr,
				       ((psli->last_iotag  + 1) *
					sizeof (struct lpfc_iocbq *)));
			psli->iocbq_lookup = new_arr;
			psli->iocbq_lookup_len = new_len;
			psli->last_iotag = iotag;
			psli->iocbq_lookup[iotag] = iocbq;
			spin_unlock_irq(&phba->hbalock);
			iocbq->iotag = iotag;
			kfree(old_arr);
			return iotag;
		}
	} else
		spin_unlock_irq(&phba->hbalock);

	lpfc_printf_log(phba, KERN_ERR,LOG_SLI,
			"0318 Failed to allocate IOTAG.last IOTAG is %d\n",
			psli->last_iotag);

	return 0;
}

static void
lpfc_sli_submit_iocb(struct lpfc_hba *phba, struct lpfc_sli_ring *pring,
		IOCB_t *iocb, struct lpfc_iocbq *nextiocb)
{
	/*
	 * Set up an iotag
	 */
	nextiocb->iocb.ulpIoTag = (nextiocb->iocb_cmpl) ? nextiocb->iotag : 0;


	if (pring->ringno == LPFC_ELS_RING) {
		lpfc_debugfs_slow_ring_trc(phba,
			"IOCB cmd ring:   wd4:x%08x wd6:x%08x wd7:x%08x",
			*(((uint32_t *) &nextiocb->iocb) + 4),
			*(((uint32_t *) &nextiocb->iocb) + 6),
			*(((uint32_t *) &nextiocb->iocb) + 7));
	}

	/*
	 * Issue iocb command to adapter
	 */
	lpfc_sli_pcimem_bcopy(&nextiocb->iocb, iocb, phba->iocb_cmd_size);
	wmb();
	pring->stats.iocb_cmd++;

	/*
	 * If there is no completion routine to call, we can release the
	 * IOCB buffer back right now. For IOCBs, like QUE_RING_BUF,
	 * that have no rsp ring completion, iocb_cmpl MUST be NULL.
	 */
	if (nextiocb->iocb_cmpl)
		lpfc_sli_ringtxcmpl_put(phba, pring, nextiocb);
	else
		__lpfc_sli_release_iocbq(phba, nextiocb);

	/*
	 * Let the HBA know what IOCB slot will be the next one the
	 * driver will put a command into.
	 */
	pring->cmdidx = pring->next_cmdidx;
	writel(pring->cmdidx, &phba->host_gp[pring->ringno].cmdPutInx);
}

static void
lpfc_sli_update_full_ring(struct lpfc_hba *phba, struct lpfc_sli_ring *pring)
{
	int ringno = pring->ringno;

	pring->flag |= LPFC_CALL_RING_AVAILABLE;

	wmb();

	/*
	 * Set ring 'ringno' to SET R0CE_REQ in Chip Att register.
	 * The HBA will tell us when an IOCB entry is available.
	 */
	writel((CA_R0ATT|CA_R0CE_REQ) << (ringno*4), phba->CAregaddr);
	readl(phba->CAregaddr); /* flush */

	pring->stats.iocb_cmd_full++;
}

static void
lpfc_sli_update_ring(struct lpfc_hba *phba, struct lpfc_sli_ring *pring)
{
	int ringno = pring->ringno;

	/*
	 * Tell the HBA that there is work to do in this ring.
	 */
	if (!(phba->sli3_options & LPFC_SLI3_CRP_ENABLED)) {
		wmb();
		writel(CA_R0ATT << (ringno * 4), phba->CAregaddr);
		readl(phba->CAregaddr); /* flush */
	}
}

static void
lpfc_sli_resume_iocb(struct lpfc_hba *phba, struct lpfc_sli_ring *pring)
{
	IOCB_t *iocb;
	struct lpfc_iocbq *nextiocb;

	/*
	 * Check to see if:
	 *  (a) there is anything on the txq to send
	 *  (b) link is up
	 *  (c) link attention events can be processed (fcp ring only)
	 *  (d) IOCB processing is not blocked by the outstanding mbox command.
	 */
	if (pring->txq_cnt &&
	    lpfc_is_link_up(phba) &&
	    (pring->ringno != phba->sli.fcp_ring ||
	     phba->sli.sli_flag & LPFC_PROCESS_LA)) {

		while ((iocb = lpfc_sli_next_iocb_slot(phba, pring)) &&
		       (nextiocb = lpfc_sli_ringtx_get(phba, pring)))
			lpfc_sli_submit_iocb(phba, pring, iocb, nextiocb);

		if (iocb)
			lpfc_sli_update_ring(phba, pring);
		else
			lpfc_sli_update_full_ring(phba, pring);
	}

	return;
}

static struct lpfc_hbq_entry *
lpfc_sli_next_hbq_slot(struct lpfc_hba *phba, uint32_t hbqno)
{
	struct hbq_s *hbqp = &phba->hbqs[hbqno];

	if (hbqp->next_hbqPutIdx == hbqp->hbqPutIdx &&
	    ++hbqp->next_hbqPutIdx >= hbqp->entry_count)
		hbqp->next_hbqPutIdx = 0;

	if (unlikely(hbqp->local_hbqGetIdx == hbqp->next_hbqPutIdx)) {
		uint32_t raw_index = phba->hbq_get[hbqno];
		uint32_t getidx = le32_to_cpu(raw_index);

		hbqp->local_hbqGetIdx = getidx;

		if (unlikely(hbqp->local_hbqGetIdx >= hbqp->entry_count)) {
			lpfc_printf_log(phba, KERN_ERR,
					LOG_SLI | LOG_VPORT,
					"1802 HBQ %d: local_hbqGetIdx "
					"%u is > than hbqp->entry_count %u\n",
					hbqno, hbqp->local_hbqGetIdx,
					hbqp->entry_count);

			phba->link_state = LPFC_HBA_ERROR;
			return NULL;
		}

		if (hbqp->local_hbqGetIdx == hbqp->next_hbqPutIdx)
			return NULL;
	}

	return (struct lpfc_hbq_entry *) phba->hbqs[hbqno].hbq_virt +
			hbqp->hbqPutIdx;
}

void
lpfc_sli_hbqbuf_free_all(struct lpfc_hba *phba)
{
	struct lpfc_dmabuf *dmabuf, *next_dmabuf;
	struct hbq_dmabuf *hbq_buf;
	unsigned long flags;
	int i, hbq_count;
	uint32_t hbqno;

	hbq_count = lpfc_sli_hbq_count();
	/* Return all memory used by all HBQs */
	spin_lock_irqsave(&phba->hbalock, flags);
	for (i = 0; i < hbq_count; ++i) {
		list_for_each_entry_safe(dmabuf, next_dmabuf,
				&phba->hbqs[i].hbq_buffer_list, list) {
			hbq_buf = container_of(dmabuf, struct hbq_dmabuf, dbuf);
			list_del(&hbq_buf->dbuf.list);
			(phba->hbqs[i].hbq_free_buffer)(phba, hbq_buf);
		}
		phba->hbqs[i].buffer_count = 0;
	}
	/* Return all HBQ buffer that are in-fly */
	list_for_each_entry_safe(dmabuf, next_dmabuf, &phba->rb_pend_list,
				 list) {
		hbq_buf = container_of(dmabuf, struct hbq_dmabuf, dbuf);
		list_del(&hbq_buf->dbuf.list);
		if (hbq_buf->tag == -1) {
			(phba->hbqs[LPFC_ELS_HBQ].hbq_free_buffer)
				(phba, hbq_buf);
		} else {
			hbqno = hbq_buf->tag >> 16;
			if (hbqno >= LPFC_MAX_HBQS)
				(phba->hbqs[LPFC_ELS_HBQ].hbq_free_buffer)
					(phba, hbq_buf);
			else
				(phba->hbqs[hbqno].hbq_free_buffer)(phba,
					hbq_buf);
		}
	}

	/* Mark the HBQs not in use */
	phba->hbq_in_use = 0;
	spin_unlock_irqrestore(&phba->hbalock, flags);
}

static int
lpfc_sli_hbq_to_firmware(struct lpfc_hba *phba, uint32_t hbqno,
			 struct hbq_dmabuf *hbq_buf)
{
	return phba->lpfc_sli_hbq_to_firmware(phba, hbqno, hbq_buf);
}

static int
lpfc_sli_hbq_to_firmware_s3(struct lpfc_hba *phba, uint32_t hbqno,
			    struct hbq_dmabuf *hbq_buf)
{
	struct lpfc_hbq_entry *hbqe;
	dma_addr_t physaddr = hbq_buf->dbuf.phys;

	/* Get next HBQ entry slot to use */
	hbqe = lpfc_sli_next_hbq_slot(phba, hbqno);
	if (hbqe) {
		struct hbq_s *hbqp = &phba->hbqs[hbqno];

		hbqe->bde.addrHigh = le32_to_cpu(putPaddrHigh(physaddr));
		hbqe->bde.addrLow  = le32_to_cpu(putPaddrLow(physaddr));
		hbqe->bde.tus.f.bdeSize = hbq_buf->size;
		hbqe->bde.tus.f.bdeFlags = 0;
		hbqe->bde.tus.w = le32_to_cpu(hbqe->bde.tus.w);
		hbqe->buffer_tag = le32_to_cpu(hbq_buf->tag);
				/* Sync SLIM */
		hbqp->hbqPutIdx = hbqp->next_hbqPutIdx;
		writel(hbqp->hbqPutIdx, phba->hbq_put + hbqno);
				/* flush */
		readl(phba->hbq_put + hbqno);
		list_add_tail(&hbq_buf->dbuf.list, &hbqp->hbq_buffer_list);
		return 0;
	} else
		return -ENOMEM;
}

static int
lpfc_sli_hbq_to_firmware_s4(struct lpfc_hba *phba, uint32_t hbqno,
			    struct hbq_dmabuf *hbq_buf)
{
	int rc;
	struct lpfc_rqe hrqe;
	struct lpfc_rqe drqe;

	hrqe.address_lo = putPaddrLow(hbq_buf->hbuf.phys);
	hrqe.address_hi = putPaddrHigh(hbq_buf->hbuf.phys);
	drqe.address_lo = putPaddrLow(hbq_buf->dbuf.phys);
	drqe.address_hi = putPaddrHigh(hbq_buf->dbuf.phys);
	rc = lpfc_sli4_rq_put(phba->sli4_hba.hdr_rq, phba->sli4_hba.dat_rq,
			      &hrqe, &drqe);
	if (rc < 0)
		return rc;
	hbq_buf->tag = rc;
	list_add_tail(&hbq_buf->dbuf.list, &phba->hbqs[hbqno].hbq_buffer_list);
	return 0;
}

/* HBQ for ELS and CT traffic. */
static struct lpfc_hbq_init lpfc_els_hbq = {
	.rn = 1,
	.entry_count = 256,
	.mask_count = 0,
	.profile = 0,
	.ring_mask = (1 << LPFC_ELS_RING),
	.buffer_count = 0,
	.init_count = 40,
	.add_count = 40,
};

/* HBQ for the extra ring if needed */
static struct lpfc_hbq_init lpfc_extra_hbq = {
	.rn = 1,
	.entry_count = 200,
	.mask_count = 0,
	.profile = 0,
	.ring_mask = (1 << LPFC_EXTRA_RING),
	.buffer_count = 0,
	.init_count = 0,
	.add_count = 5,
};

/* Array of HBQs */
struct lpfc_hbq_init *lpfc_hbq_defs[] = {
	&lpfc_els_hbq,
	&lpfc_extra_hbq,
};

static int
lpfc_sli_hbqbuf_fill_hbqs(struct lpfc_hba *phba, uint32_t hbqno, uint32_t count)
{
	uint32_t i, posted = 0;
	unsigned long flags;
	struct hbq_dmabuf *hbq_buffer;
	LIST_HEAD(hbq_buf_list);
	if (!phba->hbqs[hbqno].hbq_alloc_buffer)
		return 0;

	if ((phba->hbqs[hbqno].buffer_count + count) >
	    lpfc_hbq_defs[hbqno]->entry_count)
		count = lpfc_hbq_defs[hbqno]->entry_count -
					phba->hbqs[hbqno].buffer_count;
	if (!count)
		return 0;
	/* Allocate HBQ entries */
	for (i = 0; i < count; i++) {
		hbq_buffer = (phba->hbqs[hbqno].hbq_alloc_buffer)(phba);
		if (!hbq_buffer)
			break;
		list_add_tail(&hbq_buffer->dbuf.list, &hbq_buf_list);
	}
	/* Check whether HBQ is still in use */
	spin_lock_irqsave(&phba->hbalock, flags);
	if (!phba->hbq_in_use)
		goto err;
	while (!list_empty(&hbq_buf_list)) {
		list_remove_head(&hbq_buf_list, hbq_buffer, struct hbq_dmabuf,
				 dbuf.list);
		hbq_buffer->tag = (phba->hbqs[hbqno].buffer_count |
				      (hbqno << 16));
		if (!lpfc_sli_hbq_to_firmware(phba, hbqno, hbq_buffer)) {
			phba->hbqs[hbqno].buffer_count++;
			posted++;
		} else
			(phba->hbqs[hbqno].hbq_free_buffer)(phba, hbq_buffer);
	}
	spin_unlock_irqrestore(&phba->hbalock, flags);
	return posted;
err:
	spin_unlock_irqrestore(&phba->hbalock, flags);
	while (!list_empty(&hbq_buf_list)) {
		list_remove_head(&hbq_buf_list, hbq_buffer, struct hbq_dmabuf,
				 dbuf.list);
		(phba->hbqs[hbqno].hbq_free_buffer)(phba, hbq_buffer);
	}
	return 0;
}

int
lpfc_sli_hbqbuf_add_hbqs(struct lpfc_hba *phba, uint32_t qno)
{
	if (phba->sli_rev == LPFC_SLI_REV4)
		return 0;
	else
		return lpfc_sli_hbqbuf_fill_hbqs(phba, qno,
					 lpfc_hbq_defs[qno]->add_count);
}

static int
lpfc_sli_hbqbuf_init_hbqs(struct lpfc_hba *phba, uint32_t qno)
{
	if (phba->sli_rev == LPFC_SLI_REV4)
		return lpfc_sli_hbqbuf_fill_hbqs(phba, qno,
					 lpfc_hbq_defs[qno]->entry_count);
	else
		return lpfc_sli_hbqbuf_fill_hbqs(phba, qno,
					 lpfc_hbq_defs[qno]->init_count);
}

static struct hbq_dmabuf *
lpfc_sli_hbqbuf_get(struct list_head *rb_list)
{
	struct lpfc_dmabuf *d_buf;

	list_remove_head(rb_list, d_buf, struct lpfc_dmabuf, list);
	if (!d_buf)
		return NULL;
	return container_of(d_buf, struct hbq_dmabuf, dbuf);
}

static struct hbq_dmabuf *
lpfc_sli_hbqbuf_find(struct lpfc_hba *phba, uint32_t tag)
{
	struct lpfc_dmabuf *d_buf;
	struct hbq_dmabuf *hbq_buf;
	uint32_t hbqno;

	hbqno = tag >> 16;
	if (hbqno >= LPFC_MAX_HBQS)
		return NULL;

	spin_lock_irq(&phba->hbalock);
	list_for_each_entry(d_buf, &phba->hbqs[hbqno].hbq_buffer_list, list) {
		hbq_buf = container_of(d_buf, struct hbq_dmabuf, dbuf);
		if (hbq_buf->tag == tag) {
			spin_unlock_irq(&phba->hbalock);
			return hbq_buf;
		}
	}
	spin_unlock_irq(&phba->hbalock);
	lpfc_printf_log(phba, KERN_ERR, LOG_SLI | LOG_VPORT,
			"1803 Bad hbq tag. Data: x%x x%x\n",
			tag, phba->hbqs[tag >> 16].buffer_count);
	return NULL;
}

void
lpfc_sli_free_hbq(struct lpfc_hba *phba, struct hbq_dmabuf *hbq_buffer)
{
	uint32_t hbqno;

	if (hbq_buffer) {
		hbqno = hbq_buffer->tag >> 16;
		if (lpfc_sli_hbq_to_firmware(phba, hbqno, hbq_buffer))
			(phba->hbqs[hbqno].hbq_free_buffer)(phba, hbq_buffer);
	}
}

static int
lpfc_sli_chk_mbx_command(uint8_t mbxCommand)
{
	uint8_t ret;

	switch (mbxCommand) {
	case MBX_LOAD_SM:
	case MBX_READ_NV:
	case MBX_WRITE_NV:
	case MBX_WRITE_VPARMS:
	case MBX_RUN_BIU_DIAG:
	case MBX_INIT_LINK:
	case MBX_DOWN_LINK:
	case MBX_CONFIG_LINK:
	case MBX_CONFIG_RING:
	case MBX_RESET_RING:
	case MBX_READ_CONFIG:
	case MBX_READ_RCONFIG:
	case MBX_READ_SPARM:
	case MBX_READ_STATUS:
	case MBX_READ_RPI:
	case MBX_READ_XRI:
	case MBX_READ_REV:
	case MBX_READ_LNK_STAT:
	case MBX_REG_LOGIN:
	case MBX_UNREG_LOGIN:
	case MBX_READ_LA:
	case MBX_CLEAR_LA:
	case MBX_DUMP_MEMORY:
	case MBX_DUMP_CONTEXT:
	case MBX_RUN_DIAGS:
	case MBX_RESTART:
	case MBX_UPDATE_CFG:
	case MBX_DOWN_LOAD:
	case MBX_DEL_LD_ENTRY:
	case MBX_RUN_PROGRAM:
	case MBX_SET_MASK:
	case MBX_SET_VARIABLE:
	case MBX_UNREG_D_ID:
	case MBX_KILL_BOARD:
	case MBX_CONFIG_FARP:
	case MBX_BEACON:
	case MBX_LOAD_AREA:
	case MBX_RUN_BIU_DIAG64:
	case MBX_CONFIG_PORT:
	case MBX_READ_SPARM64:
	case MBX_READ_RPI64:
	case MBX_REG_LOGIN64:
	case MBX_READ_LA64:
	case MBX_WRITE_WWN:
	case MBX_SET_DEBUG:
	case MBX_LOAD_EXP_ROM:
	case MBX_ASYNCEVT_ENABLE:
	case MBX_REG_VPI:
	case MBX_UNREG_VPI:
	case MBX_HEARTBEAT:
	case MBX_PORT_CAPABILITIES:
	case MBX_PORT_IOV_CONTROL:
	case MBX_SLI4_CONFIG:
	case MBX_SLI4_REQ_FTRS:
	case MBX_REG_FCFI:
	case MBX_UNREG_FCFI:
	case MBX_REG_VFI:
	case MBX_UNREG_VFI:
	case MBX_INIT_VPI:
	case MBX_INIT_VFI:
	case MBX_RESUME_RPI:
	case MBX_READ_EVENT_LOG_STATUS:
	case MBX_READ_EVENT_LOG:
		ret = mbxCommand;
		break;
	default:
		ret = MBX_SHUTDOWN;
		break;
	}
	return ret;
}

void
lpfc_sli_wake_mbox_wait(struct lpfc_hba *phba, LPFC_MBOXQ_t *pmboxq)
{
	wait_queue_head_t *pdone_q;
	unsigned long drvr_flag;

	/*
	 * If pdone_q is empty, the driver thread gave up waiting and
	 * continued running.
	 */
	pmboxq->mbox_flag |= LPFC_MBX_WAKE;
	spin_lock_irqsave(&phba->hbalock, drvr_flag);
	pdone_q = (wait_queue_head_t *) pmboxq->context1;
	if (pdone_q)
		wake_up_interruptible(pdone_q);
	spin_unlock_irqrestore(&phba->hbalock, drvr_flag);
	return;
}


void
lpfc_sli_def_mbox_cmpl(struct lpfc_hba *phba, LPFC_MBOXQ_t *pmb)
{
	struct lpfc_dmabuf *mp;
	uint16_t rpi, vpi;
	int rc;
	struct lpfc_vport  *vport = pmb->vport;

	mp = (struct lpfc_dmabuf *) (pmb->context1);

	if (mp) {
		lpfc_mbuf_free(phba, mp->virt, mp->phys);
		kfree(mp);
	}

	if ((pmb->u.mb.mbxCommand == MBX_UNREG_LOGIN) &&
	    (phba->sli_rev == LPFC_SLI_REV4))
		lpfc_sli4_free_rpi(phba, pmb->u.mb.un.varUnregLogin.rpi);

	/*
	 * If a REG_LOGIN succeeded  after node is destroyed or node
	 * is in re-discovery driver need to cleanup the RPI.
	 */
	if (!(phba->pport->load_flag & FC_UNLOADING) &&
	    pmb->u.mb.mbxCommand == MBX_REG_LOGIN64 &&
	    !pmb->u.mb.mbxStatus) {
		rpi = pmb->u.mb.un.varWords[0];
		vpi = pmb->u.mb.un.varRegLogin.vpi - phba->vpi_base;
		lpfc_unreg_login(phba, vpi, rpi, pmb);
		pmb->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
		rc = lpfc_sli_issue_mbox(phba, pmb, MBX_NOWAIT);
		if (rc != MBX_NOT_FINISHED)
			return;
	}

	/* Unreg VPI, if the REG_VPI succeed after VLink failure */
	if ((pmb->u.mb.mbxCommand == MBX_REG_VPI) &&
		!(phba->pport->load_flag & FC_UNLOADING) &&
		!pmb->u.mb.mbxStatus) {
		lpfc_unreg_vpi(phba, pmb->u.mb.un.varRegVpi.vpi, pmb);
		pmb->vport = vport;
		pmb->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
		rc = lpfc_sli_issue_mbox(phba, pmb, MBX_NOWAIT);
		if (rc != MBX_NOT_FINISHED)
			return;
	}

	if (bf_get(lpfc_mqe_command, &pmb->u.mqe) == MBX_SLI4_CONFIG)
		lpfc_sli4_mbox_cmd_free(phba, pmb);
	else
		mempool_free(pmb, phba->mbox_mem_pool);
}

int
lpfc_sli_handle_mb_event(struct lpfc_hba *phba)
{
	MAILBOX_t *pmbox;
	LPFC_MBOXQ_t *pmb;
	int rc;
	LIST_HEAD(cmplq);

	phba->sli.slistat.mbox_event++;

	/* Get all completed mailboxe buffers into the cmplq */
	spin_lock_irq(&phba->hbalock);
	list_splice_init(&phba->sli.mboxq_cmpl, &cmplq);
	spin_unlock_irq(&phba->hbalock);

	/* Get a Mailbox buffer to setup mailbox commands for callback */
	do {
		list_remove_head(&cmplq, pmb, LPFC_MBOXQ_t, list);
		if (pmb == NULL)
			break;

		pmbox = &pmb->u.mb;

		if (pmbox->mbxCommand != MBX_HEARTBEAT) {
			if (pmb->vport) {
				lpfc_debugfs_disc_trc(pmb->vport,
					LPFC_DISC_TRC_MBOX_VPORT,
					"MBOX cmpl vport: cmd:x%x mb:x%x x%x",
					(uint32_t)pmbox->mbxCommand,
					pmbox->un.varWords[0],
					pmbox->un.varWords[1]);
			}
			else {
				lpfc_debugfs_disc_trc(phba->pport,
					LPFC_DISC_TRC_MBOX,
					"MBOX cmpl:       cmd:x%x mb:x%x x%x",
					(uint32_t)pmbox->mbxCommand,
					pmbox->un.varWords[0],
					pmbox->un.varWords[1]);
			}
		}

		/*
		 * It is a fatal error if unknown mbox command completion.
		 */
		if (lpfc_sli_chk_mbx_command(pmbox->mbxCommand) ==
		    MBX_SHUTDOWN) {
			/* Unknown mailbox command compl */
			lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
					"(%d):0323 Unknown Mailbox command "
					"x%x (x%x) Cmpl\n",
					pmb->vport ? pmb->vport->vpi : 0,
					pmbox->mbxCommand,
					lpfc_sli4_mbox_opcode_get(phba, pmb));
			phba->link_state = LPFC_HBA_ERROR;
			phba->work_hs = HS_FFER3;
			lpfc_handle_eratt(phba);
			continue;
		}

		if (pmbox->mbxStatus) {
			phba->sli.slistat.mbox_stat_err++;
			if (pmbox->mbxStatus == MBXERR_NO_RESOURCES) {
				/* Mbox cmd cmpl error - RETRYing */
				lpfc_printf_log(phba, KERN_INFO,
						LOG_MBOX | LOG_SLI,
						"(%d):0305 Mbox cmd cmpl "
						"error - RETRYing Data: x%x "
						"(x%x) x%x x%x x%x\n",
						pmb->vport ? pmb->vport->vpi :0,
						pmbox->mbxCommand,
						lpfc_sli4_mbox_opcode_get(phba,
									  pmb),
						pmbox->mbxStatus,
						pmbox->un.varWords[0],
						pmb->vport->port_state);
				pmbox->mbxStatus = 0;
				pmbox->mbxOwner = OWN_HOST;
				rc = lpfc_sli_issue_mbox(phba, pmb, MBX_NOWAIT);
				if (rc != MBX_NOT_FINISHED)
					continue;
			}
		}

		/* Mailbox cmd <cmd> Cmpl <cmpl> */
		lpfc_printf_log(phba, KERN_INFO, LOG_MBOX | LOG_SLI,
				"(%d):0307 Mailbox cmd x%x (x%x) Cmpl x%p "
				"Data: x%x x%x x%x x%x x%x x%x x%x x%x x%x\n",
				pmb->vport ? pmb->vport->vpi : 0,
				pmbox->mbxCommand,
				lpfc_sli4_mbox_opcode_get(phba, pmb),
				pmb->mbox_cmpl,
				*((uint32_t *) pmbox),
				pmbox->un.varWords[0],
				pmbox->un.varWords[1],
				pmbox->un.varWords[2],
				pmbox->un.varWords[3],
				pmbox->un.varWords[4],
				pmbox->un.varWords[5],
				pmbox->un.varWords[6],
				pmbox->un.varWords[7]);

		if (pmb->mbox_cmpl)
			pmb->mbox_cmpl(phba,pmb);
	} while (1);
	return 0;
}

static struct lpfc_dmabuf *
lpfc_sli_get_buff(struct lpfc_hba *phba,
		  struct lpfc_sli_ring *pring,
		  uint32_t tag)
{
	struct hbq_dmabuf *hbq_entry;

	if (tag & QUE_BUFTAG_BIT)
		return lpfc_sli_ring_taggedbuf_get(phba, pring, tag);
	hbq_entry = lpfc_sli_hbqbuf_find(phba, tag);
	if (!hbq_entry)
		return NULL;
	return &hbq_entry->dbuf;
}

static int
lpfc_complete_unsol_iocb(struct lpfc_hba *phba, struct lpfc_sli_ring *pring,
			 struct lpfc_iocbq *saveq, uint32_t fch_r_ctl,
			 uint32_t fch_type)
{
	int i;

	/* unSolicited Responses */
	if (pring->prt[0].profile) {
		if (pring->prt[0].lpfc_sli_rcv_unsol_event)
			(pring->prt[0].lpfc_sli_rcv_unsol_event) (phba, pring,
									saveq);
		return 1;
	}
	/* We must search, based on rctl / type
	   for the right routine */
	for (i = 0; i < pring->num_mask; i++) {
		if ((pring->prt[i].rctl == fch_r_ctl) &&
		    (pring->prt[i].type == fch_type)) {
			if (pring->prt[i].lpfc_sli_rcv_unsol_event)
				(pring->prt[i].lpfc_sli_rcv_unsol_event)
						(phba, pring, saveq);
			return 1;
		}
	}
	return 0;
}

static int
lpfc_sli_process_unsol_iocb(struct lpfc_hba *phba, struct lpfc_sli_ring *pring,
			    struct lpfc_iocbq *saveq)
{
	IOCB_t           * irsp;
	WORD5            * w5p;
	uint32_t           Rctl, Type;
	uint32_t           match;
	struct lpfc_iocbq *iocbq;
	struct lpfc_dmabuf *dmzbuf;

	match = 0;
	irsp = &(saveq->iocb);

	if (irsp->ulpCommand == CMD_ASYNC_STATUS) {
		if (pring->lpfc_sli_rcv_async_status)
			pring->lpfc_sli_rcv_async_status(phba, pring, saveq);
		else
			lpfc_printf_log(phba,
					KERN_WARNING,
					LOG_SLI,
					"0316 Ring %d handler: unexpected "
					"ASYNC_STATUS iocb received evt_code "
					"0x%x\n",
					pring->ringno,
					irsp->un.asyncstat.evt_code);
		return 1;
	}

	if ((irsp->ulpCommand == CMD_IOCB_RET_XRI64_CX) &&
		(phba->sli3_options & LPFC_SLI3_HBQ_ENABLED)) {
		if (irsp->ulpBdeCount > 0) {
			dmzbuf = lpfc_sli_get_buff(phba, pring,
					irsp->un.ulpWord[3]);
			lpfc_in_buf_free(phba, dmzbuf);
		}

		if (irsp->ulpBdeCount > 1) {
			dmzbuf = lpfc_sli_get_buff(phba, pring,
					irsp->unsli3.sli3Words[3]);
			lpfc_in_buf_free(phba, dmzbuf);
		}

		if (irsp->ulpBdeCount > 2) {
			dmzbuf = lpfc_sli_get_buff(phba, pring,
				irsp->unsli3.sli3Words[7]);
			lpfc_in_buf_free(phba, dmzbuf);
		}

		return 1;
	}

	if (phba->sli3_options & LPFC_SLI3_HBQ_ENABLED) {
		if (irsp->ulpBdeCount != 0) {
			saveq->context2 = lpfc_sli_get_buff(phba, pring,
						irsp->un.ulpWord[3]);
			if (!saveq->context2)
				lpfc_printf_log(phba,
					KERN_ERR,
					LOG_SLI,
					"0341 Ring %d Cannot find buffer for "
					"an unsolicited iocb. tag 0x%x\n",
					pring->ringno,
					irsp->un.ulpWord[3]);
		}
		if (irsp->ulpBdeCount == 2) {
			saveq->context3 = lpfc_sli_get_buff(phba, pring,
						irsp->unsli3.sli3Words[7]);
			if (!saveq->context3)
				lpfc_printf_log(phba,
					KERN_ERR,
					LOG_SLI,
					"0342 Ring %d Cannot find buffer for an"
					" unsolicited iocb. tag 0x%x\n",
					pring->ringno,
					irsp->unsli3.sli3Words[7]);
		}
		list_for_each_entry(iocbq, &saveq->list, list) {
			irsp = &(iocbq->iocb);
			if (irsp->ulpBdeCount != 0) {
				iocbq->context2 = lpfc_sli_get_buff(phba, pring,
							irsp->un.ulpWord[3]);
				if (!iocbq->context2)
					lpfc_printf_log(phba,
						KERN_ERR,
						LOG_SLI,
						"0343 Ring %d Cannot find "
						"buffer for an unsolicited iocb"
						". tag 0x%x\n", pring->ringno,
						irsp->un.ulpWord[3]);
			}
			if (irsp->ulpBdeCount == 2) {
				iocbq->context3 = lpfc_sli_get_buff(phba, pring,
						irsp->unsli3.sli3Words[7]);
				if (!iocbq->context3)
					lpfc_printf_log(phba,
						KERN_ERR,
						LOG_SLI,
						"0344 Ring %d Cannot find "
						"buffer for an unsolicited "
						"iocb. tag 0x%x\n",
						pring->ringno,
						irsp->unsli3.sli3Words[7]);
			}
		}
	}
	if (irsp->ulpBdeCount != 0 &&
	    (irsp->ulpCommand == CMD_IOCB_RCV_CONT64_CX ||
	     irsp->ulpStatus == IOSTAT_INTERMED_RSP)) {
		int found = 0;

		/* search continue save q for same XRI */
		list_for_each_entry(iocbq, &pring->iocb_continue_saveq, clist) {
			if (iocbq->iocb.ulpContext == saveq->iocb.ulpContext) {
				list_add_tail(&saveq->list, &iocbq->list);
				found = 1;
				break;
			}
		}
		if (!found)
			list_add_tail(&saveq->clist,
				      &pring->iocb_continue_saveq);
		if (saveq->iocb.ulpStatus != IOSTAT_INTERMED_RSP) {
			list_del_init(&iocbq->clist);
			saveq = iocbq;
			irsp = &(saveq->iocb);
		} else
			return 0;
	}
	if ((irsp->ulpCommand == CMD_RCV_ELS_REQ64_CX) ||
	    (irsp->ulpCommand == CMD_RCV_ELS_REQ_CX) ||
	    (irsp->ulpCommand == CMD_IOCB_RCV_ELS64_CX)) {
		Rctl = FC_RCTL_ELS_REQ;
		Type = FC_TYPE_ELS;
	} else {
		w5p = (WORD5 *)&(saveq->iocb.un.ulpWord[5]);
		Rctl = w5p->hcsw.Rctl;
		Type = w5p->hcsw.Type;

		/* Firmware Workaround */
		if ((Rctl == 0) && (pring->ringno == LPFC_ELS_RING) &&
			(irsp->ulpCommand == CMD_RCV_SEQUENCE64_CX ||
			 irsp->ulpCommand == CMD_IOCB_RCV_SEQ64_CX)) {
			Rctl = FC_RCTL_ELS_REQ;
			Type = FC_TYPE_ELS;
			w5p->hcsw.Rctl = Rctl;
			w5p->hcsw.Type = Type;
		}
	}

	if (!lpfc_complete_unsol_iocb(phba, pring, saveq, Rctl, Type))
		lpfc_printf_log(phba, KERN_WARNING, LOG_SLI,
				"0313 Ring %d handler: unexpected Rctl x%x "
				"Type x%x received\n",
				pring->ringno, Rctl, Type);

	return 1;
}

static struct lpfc_iocbq *
lpfc_sli_iocbq_lookup(struct lpfc_hba *phba,
		      struct lpfc_sli_ring *pring,
		      struct lpfc_iocbq *prspiocb)
{
	struct lpfc_iocbq *cmd_iocb = NULL;
	uint16_t iotag;

	iotag = prspiocb->iocb.ulpIoTag;

	if (iotag != 0 && iotag <= phba->sli.last_iotag) {
		cmd_iocb = phba->sli.iocbq_lookup[iotag];
		list_del_init(&cmd_iocb->list);
		pring->txcmplq_cnt--;
		return cmd_iocb;
	}

	lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
			"0317 iotag x%x is out off "
			"range: max iotag x%x wd0 x%x\n",
			iotag, phba->sli.last_iotag,
			*(((uint32_t *) &prspiocb->iocb) + 7));
	return NULL;
}

static struct lpfc_iocbq *
lpfc_sli_iocbq_lookup_by_tag(struct lpfc_hba *phba,
			     struct lpfc_sli_ring *pring, uint16_t iotag)
{
	struct lpfc_iocbq *cmd_iocb;

	if (iotag != 0 && iotag <= phba->sli.last_iotag) {
		cmd_iocb = phba->sli.iocbq_lookup[iotag];
		list_del_init(&cmd_iocb->list);
		pring->txcmplq_cnt--;
		return cmd_iocb;
	}

	lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
			"0372 iotag x%x is out off range: max iotag (x%x)\n",
			iotag, phba->sli.last_iotag);
	return NULL;
}

static int
lpfc_sli_process_sol_iocb(struct lpfc_hba *phba, struct lpfc_sli_ring *pring,
			  struct lpfc_iocbq *saveq)
{
	struct lpfc_iocbq *cmdiocbp;
	int rc = 1;
	unsigned long iflag;

	/* Based on the iotag field, get the cmd IOCB from the txcmplq */
	spin_lock_irqsave(&phba->hbalock, iflag);
	cmdiocbp = lpfc_sli_iocbq_lookup(phba, pring, saveq);
	spin_unlock_irqrestore(&phba->hbalock, iflag);

	if (cmdiocbp) {
		if (cmdiocbp->iocb_cmpl) {
			/*
			 * If an ELS command failed send an event to mgmt
			 * application.
			 */
			if (saveq->iocb.ulpStatus &&
			     (pring->ringno == LPFC_ELS_RING) &&
			     (cmdiocbp->iocb.ulpCommand ==
				CMD_ELS_REQUEST64_CR))
				lpfc_send_els_failure_event(phba,
					cmdiocbp, saveq);

			/*
			 * Post all ELS completions to the worker thread.
			 * All other are passed to the completion callback.
			 */
			if (pring->ringno == LPFC_ELS_RING) {
				if ((phba->sli_rev < LPFC_SLI_REV4) &&
				    (cmdiocbp->iocb_flag &
							LPFC_DRIVER_ABORTED)) {
					spin_lock_irqsave(&phba->hbalock,
							  iflag);
					cmdiocbp->iocb_flag &=
						~LPFC_DRIVER_ABORTED;
					spin_unlock_irqrestore(&phba->hbalock,
							       iflag);
					saveq->iocb.ulpStatus =
						IOSTAT_LOCAL_REJECT;
					saveq->iocb.un.ulpWord[4] =
						IOERR_SLI_ABORTED;

					/* Firmware could still be in progress
					 * of DMAing payload, so don't free data
					 * buffer till after a hbeat.
					 */
					spin_lock_irqsave(&phba->hbalock,
							  iflag);
					saveq->iocb_flag |= LPFC_DELAY_MEM_FREE;
					spin_unlock_irqrestore(&phba->hbalock,
							       iflag);
				}
				if (phba->sli_rev == LPFC_SLI_REV4) {
					if (saveq->iocb_flag &
					    LPFC_EXCHANGE_BUSY) {
						/* Set cmdiocb flag for the
						 * exchange busy so sgl (xri)
						 * will not be released until
						 * the abort xri is received
						 * from hba.
						 */
						spin_lock_irqsave(
							&phba->hbalock, iflag);
						cmdiocbp->iocb_flag |=
							LPFC_EXCHANGE_BUSY;
						spin_unlock_irqrestore(
							&phba->hbalock, iflag);
					}
					if (cmdiocbp->iocb_flag &
					    LPFC_DRIVER_ABORTED) {
						/*
						 * Clear LPFC_DRIVER_ABORTED
						 * bit in case it was driver
						 * initiated abort.
						 */
						spin_lock_irqsave(
							&phba->hbalock, iflag);
						cmdiocbp->iocb_flag &=
							~LPFC_DRIVER_ABORTED;
						spin_unlock_irqrestore(
							&phba->hbalock, iflag);
						cmdiocbp->iocb.ulpStatus =
							IOSTAT_LOCAL_REJECT;
						cmdiocbp->iocb.un.ulpWord[4] =
							IOERR_ABORT_REQUESTED;
						/*
						 * For SLI4, irsiocb contains
						 * NO_XRI in sli_xritag, it
						 * shall not affect releasing
						 * sgl (xri) process.
						 */
						saveq->iocb.ulpStatus =
							IOSTAT_LOCAL_REJECT;
						saveq->iocb.un.ulpWord[4] =
							IOERR_SLI_ABORTED;
						spin_lock_irqsave(
							&phba->hbalock, iflag);
						saveq->iocb_flag |=
							LPFC_DELAY_MEM_FREE;
						spin_unlock_irqrestore(
							&phba->hbalock, iflag);
					}
				}
			}
			(cmdiocbp->iocb_cmpl) (phba, cmdiocbp, saveq);
		} else
			lpfc_sli_release_iocbq(phba, cmdiocbp);
	} else {
		/*
		 * Unknown initiating command based on the response iotag.
		 * This could be the case on the ELS ring because of
		 * lpfc_els_abort().
		 */
		if (pring->ringno != LPFC_ELS_RING) {
			/*
			 * Ring <ringno> handler: unexpected completion IoTag
			 * <IoTag>
			 */
			lpfc_printf_log(phba, KERN_WARNING, LOG_SLI,
					 "0322 Ring %d handler: "
					 "unexpected completion IoTag x%x "
					 "Data: x%x x%x x%x x%x\n",
					 pring->ringno,
					 saveq->iocb.ulpIoTag,
					 saveq->iocb.ulpStatus,
					 saveq->iocb.un.ulpWord[4],
					 saveq->iocb.ulpCommand,
					 saveq->iocb.ulpContext);
		}
	}

	return rc;
}

static void
lpfc_sli_rsp_pointers_error(struct lpfc_hba *phba, struct lpfc_sli_ring *pring)
{
	struct lpfc_pgp *pgp = &phba->port_gp[pring->ringno];
	/*
	 * Ring <ringno> handler: portRspPut <portRspPut> is bigger than
	 * rsp ring <portRspMax>
	 */
	lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
			"0312 Ring %d handler: portRspPut %d "
			"is bigger than rsp ring %d\n",
			pring->ringno, le32_to_cpu(pgp->rspPutInx),
			pring->numRiocb);

	phba->link_state = LPFC_HBA_ERROR;

	/*
	 * All error attention handlers are posted to
	 * worker thread
	 */
	phba->work_ha |= HA_ERATT;
	phba->work_hs = HS_FFER3;

	lpfc_worker_wake_up(phba);

	return;
}

void lpfc_poll_eratt(unsigned long ptr)
{
	struct lpfc_hba *phba;
	uint32_t eratt = 0;

	phba = (struct lpfc_hba *)ptr;

	/* Check chip HA register for error event */
	eratt = lpfc_sli_check_eratt(phba);

	if (eratt)
		/* Tell the worker thread there is work to do */
		lpfc_worker_wake_up(phba);
	else
		/* Restart the timer for next eratt poll */
		mod_timer(&phba->eratt_poll, jiffies +
					HZ * LPFC_ERATT_POLL_INTERVAL);
	return;
}


int
lpfc_sli_handle_fast_ring_event(struct lpfc_hba *phba,
				struct lpfc_sli_ring *pring, uint32_t mask)
{
	struct lpfc_pgp *pgp = &phba->port_gp[pring->ringno];
	IOCB_t *irsp = NULL;
	IOCB_t *entry = NULL;
	struct lpfc_iocbq *cmdiocbq = NULL;
	struct lpfc_iocbq rspiocbq;
	uint32_t status;
	uint32_t portRspPut, portRspMax;
	int rc = 1;
	lpfc_iocb_type type;
	unsigned long iflag;
	uint32_t rsp_cmpl = 0;

	spin_lock_irqsave(&phba->hbalock, iflag);
	pring->stats.iocb_event++;

	/*
	 * The next available response entry should never exceed the maximum
	 * entries.  If it does, treat it as an adapter hardware error.
	 */
	portRspMax = pring->numRiocb;
	portRspPut = le32_to_cpu(pgp->rspPutInx);
	if (unlikely(portRspPut >= portRspMax)) {
		lpfc_sli_rsp_pointers_error(phba, pring);
		spin_unlock_irqrestore(&phba->hbalock, iflag);
		return 1;
	}
	if (phba->fcp_ring_in_use) {
		spin_unlock_irqrestore(&phba->hbalock, iflag);
		return 1;
	} else
		phba->fcp_ring_in_use = 1;

	rmb();
	while (pring->rspidx != portRspPut) {
		/*
		 * Fetch an entry off the ring and copy it into a local data
		 * structure.  The copy involves a byte-swap since the
		 * network byte order and pci byte orders are different.
		 */
		entry = lpfc_resp_iocb(phba, pring);
		phba->last_completion_time = jiffies;

		if (++pring->rspidx >= portRspMax)
			pring->rspidx = 0;

		lpfc_sli_pcimem_bcopy((uint32_t *) entry,
				      (uint32_t *) &rspiocbq.iocb,
				      phba->iocb_rsp_size);
		INIT_LIST_HEAD(&(rspiocbq.list));
		irsp = &rspiocbq.iocb;

		type = lpfc_sli_iocb_cmd_type(irsp->ulpCommand & CMD_IOCB_MASK);
		pring->stats.iocb_rsp++;
		rsp_cmpl++;

		if (unlikely(irsp->ulpStatus)) {
			/*
			 * If resource errors reported from HBA, reduce
			 * queuedepths of the SCSI device.
			 */
			if ((irsp->ulpStatus == IOSTAT_LOCAL_REJECT) &&
				(irsp->un.ulpWord[4] == IOERR_NO_RESOURCES)) {
				spin_unlock_irqrestore(&phba->hbalock, iflag);
				phba->lpfc_rampdown_queue_depth(phba);
				spin_lock_irqsave(&phba->hbalock, iflag);
			}

			/* Rsp ring <ringno> error: IOCB */
			lpfc_printf_log(phba, KERN_WARNING, LOG_SLI,
					"0336 Rsp Ring %d error: IOCB Data: "
					"x%x x%x x%x x%x x%x x%x x%x x%x\n",
					pring->ringno,
					irsp->un.ulpWord[0],
					irsp->un.ulpWord[1],
					irsp->un.ulpWord[2],
					irsp->un.ulpWord[3],
					irsp->un.ulpWord[4],
					irsp->un.ulpWord[5],
					*(uint32_t *)&irsp->un1,
					*((uint32_t *)&irsp->un1 + 1));
		}

		switch (type) {
		case LPFC_ABORT_IOCB:
		case LPFC_SOL_IOCB:
			/*
			 * Idle exchange closed via ABTS from port.  No iocb
			 * resources need to be recovered.
			 */
			if (unlikely(irsp->ulpCommand == CMD_XRI_ABORTED_CX)) {
				lpfc_printf_log(phba, KERN_INFO, LOG_SLI,
						"0333 IOCB cmd 0x%x"
						" processed. Skipping"
						" completion\n",
						irsp->ulpCommand);
				break;
			}

			cmdiocbq = lpfc_sli_iocbq_lookup(phba, pring,
							 &rspiocbq);
			if (unlikely(!cmdiocbq))
				break;
			if (cmdiocbq->iocb_flag & LPFC_DRIVER_ABORTED)
				cmdiocbq->iocb_flag &= ~LPFC_DRIVER_ABORTED;
			if (cmdiocbq->iocb_cmpl) {
				spin_unlock_irqrestore(&phba->hbalock, iflag);
				(cmdiocbq->iocb_cmpl)(phba, cmdiocbq,
						      &rspiocbq);
				spin_lock_irqsave(&phba->hbalock, iflag);
			}
			break;
		case LPFC_UNSOL_IOCB:
			spin_unlock_irqrestore(&phba->hbalock, iflag);
			lpfc_sli_process_unsol_iocb(phba, pring, &rspiocbq);
			spin_lock_irqsave(&phba->hbalock, iflag);
			break;
		default:
			if (irsp->ulpCommand == CMD_ADAPTER_MSG) {
				char adaptermsg[LPFC_MAX_ADPTMSG];
				memset(adaptermsg, 0, LPFC_MAX_ADPTMSG);
				memcpy(&adaptermsg[0], (uint8_t *) irsp,
				       MAX_MSG_DATA);
				dev_warn(&((phba->pcidev)->dev),
					 "lpfc%d: %s\n",
					 phba->brd_no, adaptermsg);
			} else {
				/* Unknown IOCB command */
				lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
						"0334 Unknown IOCB command "
						"Data: x%x, x%x x%x x%x x%x\n",
						type, irsp->ulpCommand,
						irsp->ulpStatus,
						irsp->ulpIoTag,
						irsp->ulpContext);
			}
			break;
		}

		/*
		 * The response IOCB has been processed.  Update the ring
		 * pointer in SLIM.  If the port response put pointer has not
		 * been updated, sync the pgp->rspPutInx and fetch the new port
		 * response put pointer.
		 */
		writel(pring->rspidx, &phba->host_gp[pring->ringno].rspGetInx);

		if (pring->rspidx == portRspPut)
			portRspPut = le32_to_cpu(pgp->rspPutInx);
	}

	if ((rsp_cmpl > 0) && (mask & HA_R0RE_REQ)) {
		pring->stats.iocb_rsp_full++;
		status = ((CA_R0ATT | CA_R0RE_RSP) << (pring->ringno * 4));
		writel(status, phba->CAregaddr);
		readl(phba->CAregaddr);
	}
	if ((mask & HA_R0CE_RSP) && (pring->flag & LPFC_CALL_RING_AVAILABLE)) {
		pring->flag &= ~LPFC_CALL_RING_AVAILABLE;
		pring->stats.iocb_cmd_empty++;

		/* Force update of the local copy of cmdGetInx */
		pring->local_getidx = le32_to_cpu(pgp->cmdGetInx);
		lpfc_sli_resume_iocb(phba, pring);

		if ((pring->lpfc_sli_cmd_available))
			(pring->lpfc_sli_cmd_available) (phba, pring);

	}

	phba->fcp_ring_in_use = 0;
	spin_unlock_irqrestore(&phba->hbalock, iflag);
	return rc;
}

static struct lpfc_iocbq *
lpfc_sli_sp_handle_rspiocb(struct lpfc_hba *phba, struct lpfc_sli_ring *pring,
			struct lpfc_iocbq *rspiocbp)
{
	struct lpfc_iocbq *saveq;
	struct lpfc_iocbq *cmdiocbp;
	struct lpfc_iocbq *next_iocb;
	IOCB_t *irsp = NULL;
	uint32_t free_saveq;
	uint8_t iocb_cmd_type;
	lpfc_iocb_type type;
	unsigned long iflag;
	int rc;

	spin_lock_irqsave(&phba->hbalock, iflag);
	/* First add the response iocb to the countinueq list */
	list_add_tail(&rspiocbp->list, &(pring->iocb_continueq));
	pring->iocb_continueq_cnt++;

	/* Now, determine whetehr the list is completed for processing */
	irsp = &rspiocbp->iocb;
	if (irsp->ulpLe) {
		/*
		 * By default, the driver expects to free all resources
		 * associated with this iocb completion.
		 */
		free_saveq = 1;
		saveq = list_get_first(&pring->iocb_continueq,
				       struct lpfc_iocbq, list);
		irsp = &(saveq->iocb);
		list_del_init(&pring->iocb_continueq);
		pring->iocb_continueq_cnt = 0;

		pring->stats.iocb_rsp++;

		/*
		 * If resource errors reported from HBA, reduce
		 * queuedepths of the SCSI device.
		 */
		if ((irsp->ulpStatus == IOSTAT_LOCAL_REJECT) &&
		    (irsp->un.ulpWord[4] == IOERR_NO_RESOURCES)) {
			spin_unlock_irqrestore(&phba->hbalock, iflag);
			phba->lpfc_rampdown_queue_depth(phba);
			spin_lock_irqsave(&phba->hbalock, iflag);
		}

		if (irsp->ulpStatus) {
			/* Rsp ring <ringno> error: IOCB */
			lpfc_printf_log(phba, KERN_WARNING, LOG_SLI,
					"0328 Rsp Ring %d error: "
					"IOCB Data: "
					"x%x x%x x%x x%x "
					"x%x x%x x%x x%x "
					"x%x x%x x%x x%x "
					"x%x x%x x%x x%x\n",
					pring->ringno,
					irsp->un.ulpWord[0],
					irsp->un.ulpWord[1],
					irsp->un.ulpWord[2],
					irsp->un.ulpWord[3],
					irsp->un.ulpWord[4],
					irsp->un.ulpWord[5],
					*(((uint32_t *) irsp) + 6),
					*(((uint32_t *) irsp) + 7),
					*(((uint32_t *) irsp) + 8),
					*(((uint32_t *) irsp) + 9),
					*(((uint32_t *) irsp) + 10),
					*(((uint32_t *) irsp) + 11),
					*(((uint32_t *) irsp) + 12),
					*(((uint32_t *) irsp) + 13),
					*(((uint32_t *) irsp) + 14),
					*(((uint32_t *) irsp) + 15));
		}

		/*
		 * Fetch the IOCB command type and call the correct completion
		 * routine. Solicited and Unsolicited IOCBs on the ELS ring
		 * get freed back to the lpfc_iocb_list by the discovery
		 * kernel thread.
		 */
		iocb_cmd_type = irsp->ulpCommand & CMD_IOCB_MASK;
		type = lpfc_sli_iocb_cmd_type(iocb_cmd_type);
		switch (type) {
		case LPFC_SOL_IOCB:
			spin_unlock_irqrestore(&phba->hbalock, iflag);
			rc = lpfc_sli_process_sol_iocb(phba, pring, saveq);
			spin_lock_irqsave(&phba->hbalock, iflag);
			break;

		case LPFC_UNSOL_IOCB:
			spin_unlock_irqrestore(&phba->hbalock, iflag);
			rc = lpfc_sli_process_unsol_iocb(phba, pring, saveq);
			spin_lock_irqsave(&phba->hbalock, iflag);
			if (!rc)
				free_saveq = 0;
			break;

		case LPFC_ABORT_IOCB:
			cmdiocbp = NULL;
			if (irsp->ulpCommand != CMD_XRI_ABORTED_CX)
				cmdiocbp = lpfc_sli_iocbq_lookup(phba, pring,
								 saveq);
			if (cmdiocbp) {
				/* Call the specified completion routine */
				if (cmdiocbp->iocb_cmpl) {
					spin_unlock_irqrestore(&phba->hbalock,
							       iflag);
					(cmdiocbp->iocb_cmpl)(phba, cmdiocbp,
							      saveq);
					spin_lock_irqsave(&phba->hbalock,
							  iflag);
				} else
					__lpfc_sli_release_iocbq(phba,
								 cmdiocbp);
			}
			break;

		case LPFC_UNKNOWN_IOCB:
			if (irsp->ulpCommand == CMD_ADAPTER_MSG) {
				char adaptermsg[LPFC_MAX_ADPTMSG];
				memset(adaptermsg, 0, LPFC_MAX_ADPTMSG);
				memcpy(&adaptermsg[0], (uint8_t *)irsp,
				       MAX_MSG_DATA);
				dev_warn(&((phba->pcidev)->dev),
					 "lpfc%d: %s\n",
					 phba->brd_no, adaptermsg);
			} else {
				/* Unknown IOCB command */
				lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
						"0335 Unknown IOCB "
						"command Data: x%x "
						"x%x x%x x%x\n",
						irsp->ulpCommand,
						irsp->ulpStatus,
						irsp->ulpIoTag,
						irsp->ulpContext);
			}
			break;
		}

		if (free_saveq) {
			list_for_each_entry_safe(rspiocbp, next_iocb,
						 &saveq->list, list) {
				list_del(&rspiocbp->list);
				__lpfc_sli_release_iocbq(phba, rspiocbp);
			}
			__lpfc_sli_release_iocbq(phba, saveq);
		}
		rspiocbp = NULL;
	}
	spin_unlock_irqrestore(&phba->hbalock, iflag);
	return rspiocbp;
}

void
lpfc_sli_handle_slow_ring_event(struct lpfc_hba *phba,
				struct lpfc_sli_ring *pring, uint32_t mask)
{
	phba->lpfc_sli_handle_slow_ring_event(phba, pring, mask);
}

static void
lpfc_sli_handle_slow_ring_event_s3(struct lpfc_hba *phba,
				   struct lpfc_sli_ring *pring, uint32_t mask)
{
	struct lpfc_pgp *pgp;
	IOCB_t *entry;
	IOCB_t *irsp = NULL;
	struct lpfc_iocbq *rspiocbp = NULL;
	uint32_t portRspPut, portRspMax;
	unsigned long iflag;
	uint32_t status;

	pgp = &phba->port_gp[pring->ringno];
	spin_lock_irqsave(&phba->hbalock, iflag);
	pring->stats.iocb_event++;

	/*
	 * The next available response entry should never exceed the maximum
	 * entries.  If it does, treat it as an adapter hardware error.
	 */
	portRspMax = pring->numRiocb;
	portRspPut = le32_to_cpu(pgp->rspPutInx);
	if (portRspPut >= portRspMax) {
		/*
		 * Ring <ringno> handler: portRspPut <portRspPut> is bigger than
		 * rsp ring <portRspMax>
		 */
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0303 Ring %d handler: portRspPut %d "
				"is bigger than rsp ring %d\n",
				pring->ringno, portRspPut, portRspMax);

		phba->link_state = LPFC_HBA_ERROR;
		spin_unlock_irqrestore(&phba->hbalock, iflag);

		phba->work_hs = HS_FFER3;
		lpfc_handle_eratt(phba);

		return;
	}

	rmb();
	while (pring->rspidx != portRspPut) {
		/*
		 * Build a completion list and call the appropriate handler.
		 * The process is to get the next available response iocb, get
		 * a free iocb from the list, copy the response data into the
		 * free iocb, insert to the continuation list, and update the
		 * next response index to slim.  This process makes response
		 * iocb's in the ring available to DMA as fast as possible but
		 * pays a penalty for a copy operation.  Since the iocb is
		 * only 32 bytes, this penalty is considered small relative to
		 * the PCI reads for register values and a slim write.  When
		 * the ulpLe field is set, the entire Command has been
		 * received.
		 */
		entry = lpfc_resp_iocb(phba, pring);

		phba->last_completion_time = jiffies;
		rspiocbp = __lpfc_sli_get_iocbq(phba);
		if (rspiocbp == NULL) {
			printk(KERN_ERR "%s: out of buffers! Failing "
			       "completion.\n", __func__);
			break;
		}

		lpfc_sli_pcimem_bcopy(entry, &rspiocbp->iocb,
				      phba->iocb_rsp_size);
		irsp = &rspiocbp->iocb;

		if (++pring->rspidx >= portRspMax)
			pring->rspidx = 0;

		if (pring->ringno == LPFC_ELS_RING) {
			lpfc_debugfs_slow_ring_trc(phba,
			"IOCB rsp ring:   wd4:x%08x wd6:x%08x wd7:x%08x",
				*(((uint32_t *) irsp) + 4),
				*(((uint32_t *) irsp) + 6),
				*(((uint32_t *) irsp) + 7));
		}

		writel(pring->rspidx, &phba->host_gp[pring->ringno].rspGetInx);

		spin_unlock_irqrestore(&phba->hbalock, iflag);
		/* Handle the response IOCB */
		rspiocbp = lpfc_sli_sp_handle_rspiocb(phba, pring, rspiocbp);
		spin_lock_irqsave(&phba->hbalock, iflag);

		/*
		 * If the port response put pointer has not been updated, sync
		 * the pgp->rspPutInx in the MAILBOX_tand fetch the new port
		 * response put pointer.
		 */
		if (pring->rspidx == portRspPut) {
			portRspPut = le32_to_cpu(pgp->rspPutInx);
		}
	} /* while (pring->rspidx != portRspPut) */

	if ((rspiocbp != NULL) && (mask & HA_R0RE_REQ)) {
		/* At least one response entry has been freed */
		pring->stats.iocb_rsp_full++;
		/* SET RxRE_RSP in Chip Att register */
		status = ((CA_R0ATT | CA_R0RE_RSP) << (pring->ringno * 4));
		writel(status, phba->CAregaddr);
		readl(phba->CAregaddr); /* flush */
	}
	if ((mask & HA_R0CE_RSP) && (pring->flag & LPFC_CALL_RING_AVAILABLE)) {
		pring->flag &= ~LPFC_CALL_RING_AVAILABLE;
		pring->stats.iocb_cmd_empty++;

		/* Force update of the local copy of cmdGetInx */
		pring->local_getidx = le32_to_cpu(pgp->cmdGetInx);
		lpfc_sli_resume_iocb(phba, pring);

		if ((pring->lpfc_sli_cmd_available))
			(pring->lpfc_sli_cmd_available) (phba, pring);

	}

	spin_unlock_irqrestore(&phba->hbalock, iflag);
	return;
}

static void
lpfc_sli_handle_slow_ring_event_s4(struct lpfc_hba *phba,
				   struct lpfc_sli_ring *pring, uint32_t mask)
{
	struct lpfc_iocbq *irspiocbq;
	struct hbq_dmabuf *dmabuf;
	struct lpfc_cq_event *cq_event;
	unsigned long iflag;

	spin_lock_irqsave(&phba->hbalock, iflag);
	phba->hba_flag &= ~HBA_SP_QUEUE_EVT;
	spin_unlock_irqrestore(&phba->hbalock, iflag);
	while (!list_empty(&phba->sli4_hba.sp_queue_event)) {
		/* Get the response iocb from the head of work queue */
		spin_lock_irqsave(&phba->hbalock, iflag);
		list_remove_head(&phba->sli4_hba.sp_queue_event,
				 cq_event, struct lpfc_cq_event, list);
		spin_unlock_irqrestore(&phba->hbalock, iflag);

		switch (bf_get(lpfc_wcqe_c_code, &cq_event->cqe.wcqe_cmpl)) {
		case CQE_CODE_COMPL_WQE:
			irspiocbq = container_of(cq_event, struct lpfc_iocbq,
						 cq_event);
			/* Translate ELS WCQE to response IOCBQ */
			irspiocbq = lpfc_sli4_els_wcqe_to_rspiocbq(phba,
								   irspiocbq);
			if (irspiocbq)
				lpfc_sli_sp_handle_rspiocb(phba, pring,
							   irspiocbq);
			break;
		case CQE_CODE_RECEIVE:
			dmabuf = container_of(cq_event, struct hbq_dmabuf,
					      cq_event);
			lpfc_sli4_handle_received_buffer(phba, dmabuf);
			break;
		default:
			break;
		}
	}
}

void
lpfc_sli_abort_iocb_ring(struct lpfc_hba *phba, struct lpfc_sli_ring *pring)
{
	LIST_HEAD(completions);
	struct lpfc_iocbq *iocb, *next_iocb;

	if (pring->ringno == LPFC_ELS_RING) {
		lpfc_fabric_abort_hba(phba);
	}

	/* Error everything on txq and txcmplq
	 * First do the txq.
	 */
	spin_lock_irq(&phba->hbalock);
	list_splice_init(&pring->txq, &completions);
	pring->txq_cnt = 0;

	/* Next issue ABTS for everything on the txcmplq */
	list_for_each_entry_safe(iocb, next_iocb, &pring->txcmplq, list)
		lpfc_sli_issue_abort_iotag(phba, pring, iocb);

	spin_unlock_irq(&phba->hbalock);

	/* Cancel all the IOCBs from the completions list */
	lpfc_sli_cancel_iocbs(phba, &completions, IOSTAT_LOCAL_REJECT,
			      IOERR_SLI_ABORTED);
}

void
lpfc_sli_flush_fcp_rings(struct lpfc_hba *phba)
{
	LIST_HEAD(txq);
	LIST_HEAD(txcmplq);
	struct lpfc_sli *psli = &phba->sli;
	struct lpfc_sli_ring  *pring;

	/* Currently, only one fcp ring */
	pring = &psli->ring[psli->fcp_ring];

	spin_lock_irq(&phba->hbalock);
	/* Retrieve everything on txq */
	list_splice_init(&pring->txq, &txq);
	pring->txq_cnt = 0;

	/* Retrieve everything on the txcmplq */
	list_splice_init(&pring->txcmplq, &txcmplq);
	pring->txcmplq_cnt = 0;
	spin_unlock_irq(&phba->hbalock);

	/* Flush the txq */
	lpfc_sli_cancel_iocbs(phba, &txq, IOSTAT_LOCAL_REJECT,
			      IOERR_SLI_DOWN);

	/* Flush the txcmpq */
	lpfc_sli_cancel_iocbs(phba, &txcmplq, IOSTAT_LOCAL_REJECT,
			      IOERR_SLI_DOWN);
}

static int
lpfc_sli_brdready_s3(struct lpfc_hba *phba, uint32_t mask)
{
	uint32_t status;
	int i = 0;
	int retval = 0;

	/* Read the HBA Host Status Register */
	status = readl(phba->HSregaddr);

	/*
	 * Check status register every 100ms for 5 retries, then every
	 * 500ms for 5, then every 2.5 sec for 5, then reset board and
	 * every 2.5 sec for 4.
	 * Break our of the loop if errors occurred during init.
	 */
	while (((status & mask) != mask) &&
	       !(status & HS_FFERM) &&
	       i++ < 20) {

		if (i <= 5)
			msleep(10);
		else if (i <= 10)
			msleep(500);
		else
			msleep(2500);

		if (i == 15) {
				/* Do post */
			phba->pport->port_state = LPFC_VPORT_UNKNOWN;
			lpfc_sli_brdrestart(phba);
		}
		/* Read the HBA Host Status Register */
		status = readl(phba->HSregaddr);
	}

	/* Check to see if any errors occurred during init */
	if ((status & HS_FFERM) || (i >= 20)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2751 Adapter failed to restart, "
				"status reg x%x, FW Data: A8 x%x AC x%x\n",
				status,
				readl(phba->MBslimaddr + 0xa8),
				readl(phba->MBslimaddr + 0xac));
		phba->link_state = LPFC_HBA_ERROR;
		retval = 1;
	}

	return retval;
}

static int
lpfc_sli_brdready_s4(struct lpfc_hba *phba, uint32_t mask)
{
	uint32_t status;
	int retval = 0;

	/* Read the HBA Host Status Register */
	status = lpfc_sli4_post_status_check(phba);

	if (status) {
		phba->pport->port_state = LPFC_VPORT_UNKNOWN;
		lpfc_sli_brdrestart(phba);
		status = lpfc_sli4_post_status_check(phba);
	}

	/* Check to see if any errors occurred during init */
	if (status) {
		phba->link_state = LPFC_HBA_ERROR;
		retval = 1;
	} else
		phba->sli4_hba.intr_enable = 0;

	return retval;
}

int
lpfc_sli_brdready(struct lpfc_hba *phba, uint32_t mask)
{
	return phba->lpfc_sli_brdready(phba, mask);
}

#define BARRIER_TEST_PATTERN (0xdeadbeef)

void lpfc_reset_barrier(struct lpfc_hba *phba)
{
	uint32_t __iomem *resp_buf;
	uint32_t __iomem *mbox_buf;
	volatile uint32_t mbox;
	uint32_t hc_copy;
	int  i;
	uint8_t hdrtype;

	pci_read_config_byte(phba->pcidev, PCI_HEADER_TYPE, &hdrtype);
	if (hdrtype != 0x80 ||
	    (FC_JEDEC_ID(phba->vpd.rev.biuRev) != HELIOS_JEDEC_ID &&
	     FC_JEDEC_ID(phba->vpd.rev.biuRev) != THOR_JEDEC_ID))
		return;

	/*
	 * Tell the other part of the chip to suspend temporarily all
	 * its DMA activity.
	 */
	resp_buf = phba->MBslimaddr;

	/* Disable the error attention */
	hc_copy = readl(phba->HCregaddr);
	writel((hc_copy & ~HC_ERINT_ENA), phba->HCregaddr);
	readl(phba->HCregaddr); /* flush */
	phba->link_flag |= LS_IGNORE_ERATT;

	if (readl(phba->HAregaddr) & HA_ERATT) {
		/* Clear Chip error bit */
		writel(HA_ERATT, phba->HAregaddr);
		phba->pport->stopped = 1;
	}

	mbox = 0;
	((MAILBOX_t *)&mbox)->mbxCommand = MBX_KILL_BOARD;
	((MAILBOX_t *)&mbox)->mbxOwner = OWN_CHIP;

	writel(BARRIER_TEST_PATTERN, (resp_buf + 1));
	mbox_buf = phba->MBslimaddr;
	writel(mbox, mbox_buf);

	for (i = 0;
	     readl(resp_buf + 1) != ~(BARRIER_TEST_PATTERN) && i < 50; i++)
		mdelay(1);

	if (readl(resp_buf + 1) != ~(BARRIER_TEST_PATTERN)) {
		if (phba->sli.sli_flag & LPFC_SLI_ACTIVE ||
		    phba->pport->stopped)
			goto restore_hc;
		else
			goto clear_errat;
	}

	((MAILBOX_t *)&mbox)->mbxOwner = OWN_HOST;
	for (i = 0; readl(resp_buf) != mbox &&  i < 500; i++)
		mdelay(1);

clear_errat:

	while (!(readl(phba->HAregaddr) & HA_ERATT) && ++i < 500)
		mdelay(1);

	if (readl(phba->HAregaddr) & HA_ERATT) {
		writel(HA_ERATT, phba->HAregaddr);
		phba->pport->stopped = 1;
	}

restore_hc:
	phba->link_flag &= ~LS_IGNORE_ERATT;
	writel(hc_copy, phba->HCregaddr);
	readl(phba->HCregaddr); /* flush */
}

int
lpfc_sli_brdkill(struct lpfc_hba *phba)
{
	struct lpfc_sli *psli;
	LPFC_MBOXQ_t *pmb;
	uint32_t status;
	uint32_t ha_copy;
	int retval;
	int i = 0;

	psli = &phba->sli;

	/* Kill HBA */
	lpfc_printf_log(phba, KERN_INFO, LOG_SLI,
			"0329 Kill HBA Data: x%x x%x\n",
			phba->pport->port_state, psli->sli_flag);

	pmb = (LPFC_MBOXQ_t *) mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!pmb)
		return 1;

	/* Disable the error attention */
	spin_lock_irq(&phba->hbalock);
	status = readl(phba->HCregaddr);
	status &= ~HC_ERINT_ENA;
	writel(status, phba->HCregaddr);
	readl(phba->HCregaddr); /* flush */
	phba->link_flag |= LS_IGNORE_ERATT;
	spin_unlock_irq(&phba->hbalock);

	lpfc_kill_board(phba, pmb);
	pmb->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
	retval = lpfc_sli_issue_mbox(phba, pmb, MBX_NOWAIT);

	if (retval != MBX_SUCCESS) {
		if (retval != MBX_BUSY)
			mempool_free(pmb, phba->mbox_mem_pool);
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"2752 KILL_BOARD command failed retval %d\n",
				retval);
		spin_lock_irq(&phba->hbalock);
		phba->link_flag &= ~LS_IGNORE_ERATT;
		spin_unlock_irq(&phba->hbalock);
		return 1;
	}

	spin_lock_irq(&phba->hbalock);
	psli->sli_flag &= ~LPFC_SLI_ACTIVE;
	spin_unlock_irq(&phba->hbalock);

	mempool_free(pmb, phba->mbox_mem_pool);

	/* There is no completion for a KILL_BOARD mbox cmd. Check for an error
	 * attention every 100ms for 3 seconds. If we don't get ERATT after
	 * 3 seconds we still set HBA_ERROR state because the status of the
	 * board is now undefined.
	 */
	ha_copy = readl(phba->HAregaddr);

	while ((i++ < 30) && !(ha_copy & HA_ERATT)) {
		mdelay(100);
		ha_copy = readl(phba->HAregaddr);
	}

	del_timer_sync(&psli->mbox_tmo);
	if (ha_copy & HA_ERATT) {
		writel(HA_ERATT, phba->HAregaddr);
		phba->pport->stopped = 1;
	}
	spin_lock_irq(&phba->hbalock);
	psli->sli_flag &= ~LPFC_SLI_MBOX_ACTIVE;
	psli->mbox_active = NULL;
	phba->link_flag &= ~LS_IGNORE_ERATT;
	spin_unlock_irq(&phba->hbalock);

	lpfc_hba_down_post(phba);
	phba->link_state = LPFC_HBA_ERROR;

	return ha_copy & HA_ERATT ? 0 : 1;
}

int
lpfc_sli_brdreset(struct lpfc_hba *phba)
{
	struct lpfc_sli *psli;
	struct lpfc_sli_ring *pring;
	uint16_t cfg_value;
	int i;

	psli = &phba->sli;

	/* Reset HBA */
	lpfc_printf_log(phba, KERN_INFO, LOG_SLI,
			"0325 Reset HBA Data: x%x x%x\n",
			phba->pport->port_state, psli->sli_flag);

	/* perform board reset */
	phba->fc_eventTag = 0;
	phba->link_events = 0;
	phba->pport->fc_myDID = 0;
	phba->pport->fc_prevDID = 0;

	/* Turn off parity checking and serr during the physical reset */
	pci_read_config_word(phba->pcidev, PCI_COMMAND, &cfg_value);
	pci_write_config_word(phba->pcidev, PCI_COMMAND,
			      (cfg_value &
			       ~(PCI_COMMAND_PARITY | PCI_COMMAND_SERR)));

	psli->sli_flag &= ~(LPFC_SLI_ACTIVE | LPFC_PROCESS_LA);

	/* Now toggle INITFF bit in the Host Control Register */
	writel(HC_INITFF, phba->HCregaddr);
	mdelay(1);
	readl(phba->HCregaddr); /* flush */
	writel(0, phba->HCregaddr);
	readl(phba->HCregaddr); /* flush */

	/* Restore PCI cmd register */
	pci_write_config_word(phba->pcidev, PCI_COMMAND, cfg_value);

	/* Initialize relevant SLI info */
	for (i = 0; i < psli->num_rings; i++) {
		pring = &psli->ring[i];
		pring->flag = 0;
		pring->rspidx = 0;
		pring->next_cmdidx  = 0;
		pring->local_getidx = 0;
		pring->cmdidx = 0;
		pring->missbufcnt = 0;
	}

	phba->link_state = LPFC_WARM_START;
	return 0;
}

int
lpfc_sli4_brdreset(struct lpfc_hba *phba)
{
	struct lpfc_sli *psli = &phba->sli;
	uint16_t cfg_value;
	uint8_t qindx;

	/* Reset HBA */
	lpfc_printf_log(phba, KERN_INFO, LOG_SLI,
			"0295 Reset HBA Data: x%x x%x\n",
			phba->pport->port_state, psli->sli_flag);

	/* perform board reset */
	phba->fc_eventTag = 0;
	phba->link_events = 0;
	phba->pport->fc_myDID = 0;
	phba->pport->fc_prevDID = 0;

	/* Turn off parity checking and serr during the physical reset */
	pci_read_config_word(phba->pcidev, PCI_COMMAND, &cfg_value);
	pci_write_config_word(phba->pcidev, PCI_COMMAND,
			      (cfg_value &
			      ~(PCI_COMMAND_PARITY | PCI_COMMAND_SERR)));

	spin_lock_irq(&phba->hbalock);
	psli->sli_flag &= ~(LPFC_PROCESS_LA);
	phba->fcf.fcf_flag = 0;
	/* Clean up the child queue list for the CQs */
	list_del_init(&phba->sli4_hba.mbx_wq->list);
	list_del_init(&phba->sli4_hba.els_wq->list);
	list_del_init(&phba->sli4_hba.hdr_rq->list);
	list_del_init(&phba->sli4_hba.dat_rq->list);
	list_del_init(&phba->sli4_hba.mbx_cq->list);
	list_del_init(&phba->sli4_hba.els_cq->list);
	for (qindx = 0; qindx < phba->cfg_fcp_wq_count; qindx++)
		list_del_init(&phba->sli4_hba.fcp_wq[qindx]->list);
	for (qindx = 0; qindx < phba->cfg_fcp_eq_count; qindx++)
		list_del_init(&phba->sli4_hba.fcp_cq[qindx]->list);
	spin_unlock_irq(&phba->hbalock);

	/* Now physically reset the device */
	lpfc_printf_log(phba, KERN_INFO, LOG_INIT,
			"0389 Performing PCI function reset!\n");
	/* Perform FCoE PCI function reset */
	lpfc_pci_function_reset(phba);

	return 0;
}

static int
lpfc_sli_brdrestart_s3(struct lpfc_hba *phba)
{
	MAILBOX_t *mb;
	struct lpfc_sli *psli;
	volatile uint32_t word0;
	void __iomem *to_slim;
	uint32_t hba_aer_enabled;

	spin_lock_irq(&phba->hbalock);

	/* Take PCIe device Advanced Error Reporting (AER) state */
	hba_aer_enabled = phba->hba_flag & HBA_AER_ENABLED;

	psli = &phba->sli;

	/* Restart HBA */
	lpfc_printf_log(phba, KERN_INFO, LOG_SLI,
			"0337 Restart HBA Data: x%x x%x\n",
			phba->pport->port_state, psli->sli_flag);

	word0 = 0;
	mb = (MAILBOX_t *) &word0;
	mb->mbxCommand = MBX_RESTART;
	mb->mbxHc = 1;

	lpfc_reset_barrier(phba);

	to_slim = phba->MBslimaddr;
	writel(*(uint32_t *) mb, to_slim);
	readl(to_slim); /* flush */

	/* Only skip post after fc_ffinit is completed */
	if (phba->pport->port_state)
		word0 = 1;	/* This is really setting up word1 */
	else
		word0 = 0;	/* This is really setting up word1 */
	to_slim = phba->MBslimaddr + sizeof (uint32_t);
	writel(*(uint32_t *) mb, to_slim);
	readl(to_slim); /* flush */

	lpfc_sli_brdreset(phba);
	phba->pport->stopped = 0;
	phba->link_state = LPFC_INIT_START;
	phba->hba_flag = 0;
	spin_unlock_irq(&phba->hbalock);

	memset(&psli->lnk_stat_offsets, 0, sizeof(psli->lnk_stat_offsets));
	psli->stats_start = get_seconds();

	/* Give the INITFF and Post time to settle. */
	mdelay(100);

	/* Reset HBA AER if it was enabled, note hba_flag was reset above */
	if (hba_aer_enabled)
		pci_disable_pcie_error_reporting(phba->pcidev);

	lpfc_hba_down_post(phba);

	return 0;
}

static int
lpfc_sli_brdrestart_s4(struct lpfc_hba *phba)
{
	struct lpfc_sli *psli = &phba->sli;


	/* Restart HBA */
	lpfc_printf_log(phba, KERN_INFO, LOG_SLI,
			"0296 Restart HBA Data: x%x x%x\n",
			phba->pport->port_state, psli->sli_flag);

	lpfc_sli4_brdreset(phba);

	spin_lock_irq(&phba->hbalock);
	phba->pport->stopped = 0;
	phba->link_state = LPFC_INIT_START;
	phba->hba_flag = 0;
	spin_unlock_irq(&phba->hbalock);

	memset(&psli->lnk_stat_offsets, 0, sizeof(psli->lnk_stat_offsets));
	psli->stats_start = get_seconds();

	lpfc_hba_down_post(phba);

	return 0;
}

int
lpfc_sli_brdrestart(struct lpfc_hba *phba)
{
	return phba->lpfc_sli_brdrestart(phba);
}

static int
lpfc_sli_chipset_init(struct lpfc_hba *phba)
{
	uint32_t status, i = 0;

	/* Read the HBA Host Status Register */
	status = readl(phba->HSregaddr);

	/* Check status register to see what current state is */
	i = 0;
	while ((status & (HS_FFRDY | HS_MBRDY)) != (HS_FFRDY | HS_MBRDY)) {

		/* Check every 100ms for 5 retries, then every 500ms for 5, then
		 * every 2.5 sec for 5, then reset board and every 2.5 sec for
		 * 4.
		 */
		if (i++ >= 20) {
			/* Adapter failed to init, timeout, status reg
			   <status> */
			lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
					"0436 Adapter failed to init, "
					"timeout, status reg x%x, "
					"FW Data: A8 x%x AC x%x\n", status,
					readl(phba->MBslimaddr + 0xa8),
					readl(phba->MBslimaddr + 0xac));
			phba->link_state = LPFC_HBA_ERROR;
			return -ETIMEDOUT;
		}

		/* Check to see if any errors occurred during init */
		if (status & HS_FFERM) {
			/* ERROR: During chipset initialization */
			/* Adapter failed to init, chipset, status reg
			   <status> */
			lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
					"0437 Adapter failed to init, "
					"chipset, status reg x%x, "
					"FW Data: A8 x%x AC x%x\n", status,
					readl(phba->MBslimaddr + 0xa8),
					readl(phba->MBslimaddr + 0xac));
			phba->link_state = LPFC_HBA_ERROR;
			return -EIO;
		}

		if (i <= 5) {
			msleep(10);
		} else if (i <= 10) {
			msleep(500);
		} else {
			msleep(2500);
		}

		if (i == 15) {
				/* Do post */
			phba->pport->port_state = LPFC_VPORT_UNKNOWN;
			lpfc_sli_brdrestart(phba);
		}
		/* Read the HBA Host Status Register */
		status = readl(phba->HSregaddr);
	}

	/* Check to see if any errors occurred during init */
	if (status & HS_FFERM) {
		/* ERROR: During chipset initialization */
		/* Adapter failed to init, chipset, status reg <status> */
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"0438 Adapter failed to init, chipset, "
				"status reg x%x, "
				"FW Data: A8 x%x AC x%x\n", status,
				readl(phba->MBslimaddr + 0xa8),
				readl(phba->MBslimaddr + 0xac));
		phba->link_state = LPFC_HBA_ERROR;
		return -EIO;
	}

	/* Clear all interrupt enable conditions */
	writel(0, phba->HCregaddr);
	readl(phba->HCregaddr); /* flush */

	/* setup host attn register */
	writel(0xffffffff, phba->HAregaddr);
	readl(phba->HAregaddr); /* flush */
	return 0;
}

int
lpfc_sli_hbq_count(void)
{
	return ARRAY_SIZE(lpfc_hbq_defs);
}

static int
lpfc_sli_hbq_entry_count(void)
{
	int  hbq_count = lpfc_sli_hbq_count();
	int  count = 0;
	int  i;

	for (i = 0; i < hbq_count; ++i)
		count += lpfc_hbq_defs[i]->entry_count;
	return count;
}

int
lpfc_sli_hbq_size(void)
{
	return lpfc_sli_hbq_entry_count() * sizeof(struct lpfc_hbq_entry);
}

static int
lpfc_sli_hbq_setup(struct lpfc_hba *phba)
{
	int  hbq_count = lpfc_sli_hbq_count();
	LPFC_MBOXQ_t *pmb;
	MAILBOX_t *pmbox;
	uint32_t hbqno;
	uint32_t hbq_entry_index;

				/* Get a Mailbox buffer to setup mailbox
				 * commands for HBA initialization
				 */
	pmb = (LPFC_MBOXQ_t *) mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);

	if (!pmb)
		return -ENOMEM;

	pmbox = &pmb->u.mb;

	/* Initialize the struct lpfc_sli_hbq structure for each hbq */
	phba->link_state = LPFC_INIT_MBX_CMDS;
	phba->hbq_in_use = 1;

	hbq_entry_index = 0;
	for (hbqno = 0; hbqno < hbq_count; ++hbqno) {
		phba->hbqs[hbqno].next_hbqPutIdx = 0;
		phba->hbqs[hbqno].hbqPutIdx      = 0;
		phba->hbqs[hbqno].local_hbqGetIdx   = 0;
		phba->hbqs[hbqno].entry_count =
			lpfc_hbq_defs[hbqno]->entry_count;
		lpfc_config_hbq(phba, hbqno, lpfc_hbq_defs[hbqno],
			hbq_entry_index, pmb);
		hbq_entry_index += phba->hbqs[hbqno].entry_count;

		if (lpfc_sli_issue_mbox(phba, pmb, MBX_POLL) != MBX_SUCCESS) {
			/* Adapter failed to init, mbxCmd <cmd> CFG_RING,
			   mbxStatus <status>, ring <num> */

			lpfc_printf_log(phba, KERN_ERR,
					LOG_SLI | LOG_VPORT,
					"1805 Adapter failed to init. "
					"Data: x%x x%x x%x\n",
					pmbox->mbxCommand,
					pmbox->mbxStatus, hbqno);

			phba->link_state = LPFC_HBA_ERROR;
			mempool_free(pmb, phba->mbox_mem_pool);
			return ENXIO;
		}
	}
	phba->hbq_count = hbq_count;

	mempool_free(pmb, phba->mbox_mem_pool);

	/* Initially populate or replenish the HBQs */
	for (hbqno = 0; hbqno < hbq_count; ++hbqno)
		lpfc_sli_hbqbuf_init_hbqs(phba, hbqno);
	return 0;
}

static int
lpfc_sli4_rb_setup(struct lpfc_hba *phba)
{
	phba->hbq_in_use = 1;
	phba->hbqs[0].entry_count = lpfc_hbq_defs[0]->entry_count;
	phba->hbq_count = 1;
	/* Initially populate or replenish the HBQs */
	lpfc_sli_hbqbuf_init_hbqs(phba, 0);
	return 0;
}

int
lpfc_sli_config_port(struct lpfc_hba *phba, int sli_mode)
{
	LPFC_MBOXQ_t *pmb;
	uint32_t resetcount = 0, rc = 0, done = 0;

	pmb = (LPFC_MBOXQ_t *) mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!pmb) {
		phba->link_state = LPFC_HBA_ERROR;
		return -ENOMEM;
	}

	phba->sli_rev = sli_mode;
	while (resetcount < 2 && !done) {
		spin_lock_irq(&phba->hbalock);
		phba->sli.sli_flag |= LPFC_SLI_MBOX_ACTIVE;
		spin_unlock_irq(&phba->hbalock);
		phba->pport->port_state = LPFC_VPORT_UNKNOWN;
		lpfc_sli_brdrestart(phba);
		rc = lpfc_sli_chipset_init(phba);
		if (rc)
			break;

		spin_lock_irq(&phba->hbalock);
		phba->sli.sli_flag &= ~LPFC_SLI_MBOX_ACTIVE;
		spin_unlock_irq(&phba->hbalock);
		resetcount++;

		/* Call pre CONFIG_PORT mailbox command initialization.  A
		 * value of 0 means the call was successful.  Any other
		 * nonzero value is a failure, but if ERESTART is returned,
		 * the driver may reset the HBA and try again.
		 */
		rc = lpfc_config_port_prep(phba);
		if (rc == -ERESTART) {
			phba->link_state = LPFC_LINK_UNKNOWN;
			continue;
		} else if (rc)
			break;
		phba->link_state = LPFC_INIT_MBX_CMDS;
		lpfc_config_port(phba, pmb);
		rc = lpfc_sli_issue_mbox(phba, pmb, MBX_POLL);
		phba->sli3_options &= ~(LPFC_SLI3_NPIV_ENABLED |
					LPFC_SLI3_HBQ_ENABLED |
					LPFC_SLI3_CRP_ENABLED |
					LPFC_SLI3_INB_ENABLED |
					LPFC_SLI3_BG_ENABLED);
		if (rc != MBX_SUCCESS) {
			lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"0442 Adapter failed to init, mbxCmd x%x "
				"CONFIG_PORT, mbxStatus x%x Data: x%x\n",
				pmb->u.mb.mbxCommand, pmb->u.mb.mbxStatus, 0);
			spin_lock_irq(&phba->hbalock);
			phba->sli.sli_flag &= ~LPFC_SLI_ACTIVE;
			spin_unlock_irq(&phba->hbalock);
			rc = -ENXIO;
		} else {
			/* Allow asynchronous mailbox command to go through */
			spin_lock_irq(&phba->hbalock);
			phba->sli.sli_flag &= ~LPFC_SLI_ASYNC_MBX_BLK;
			spin_unlock_irq(&phba->hbalock);
			done = 1;
		}
	}
	if (!done) {
		rc = -EINVAL;
		goto do_prep_failed;
	}
	if (pmb->u.mb.un.varCfgPort.sli_mode == 3) {
		if (!pmb->u.mb.un.varCfgPort.cMA) {
			rc = -ENXIO;
			goto do_prep_failed;
		}
		if (phba->max_vpi && pmb->u.mb.un.varCfgPort.gmv) {
			phba->sli3_options |= LPFC_SLI3_NPIV_ENABLED;
			phba->max_vpi = pmb->u.mb.un.varCfgPort.max_vpi;
			phba->max_vports = (phba->max_vpi > phba->max_vports) ?
				phba->max_vpi : phba->max_vports;

		} else
			phba->max_vpi = 0;
		if (pmb->u.mb.un.varCfgPort.gdss)
			phba->sli3_options |= LPFC_SLI3_DSS_ENABLED;
		if (pmb->u.mb.un.varCfgPort.gerbm)
			phba->sli3_options |= LPFC_SLI3_HBQ_ENABLED;
		if (pmb->u.mb.un.varCfgPort.gcrp)
			phba->sli3_options |= LPFC_SLI3_CRP_ENABLED;
		if (pmb->u.mb.un.varCfgPort.ginb) {
			phba->sli3_options |= LPFC_SLI3_INB_ENABLED;
			phba->hbq_get = phba->mbox->us.s3_inb_pgp.hbq_get;
			phba->port_gp = phba->mbox->us.s3_inb_pgp.port;
			phba->inb_ha_copy = &phba->mbox->us.s3_inb_pgp.ha_copy;
			phba->inb_counter = &phba->mbox->us.s3_inb_pgp.counter;
			phba->inb_last_counter =
					phba->mbox->us.s3_inb_pgp.counter;
		} else {
			phba->hbq_get = phba->mbox->us.s3_pgp.hbq_get;
			phba->port_gp = phba->mbox->us.s3_pgp.port;
			phba->inb_ha_copy = NULL;
			phba->inb_counter = NULL;
		}

		if (phba->cfg_enable_bg) {
			if (pmb->u.mb.un.varCfgPort.gbg)
				phba->sli3_options |= LPFC_SLI3_BG_ENABLED;
			else
				lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
						"0443 Adapter did not grant "
						"BlockGuard\n");
		}
	} else {
		phba->hbq_get = NULL;
		phba->port_gp = phba->mbox->us.s2.port;
		phba->inb_ha_copy = NULL;
		phba->inb_counter = NULL;
		phba->max_vpi = 0;
	}
do_prep_failed:
	mempool_free(pmb, phba->mbox_mem_pool);
	return rc;
}


int
lpfc_sli_hba_setup(struct lpfc_hba *phba)
{
	uint32_t rc;
	int  mode = 3;

	switch (lpfc_sli_mode) {
	case 2:
		if (phba->cfg_enable_npiv) {
			lpfc_printf_log(phba, KERN_ERR, LOG_INIT | LOG_VPORT,
				"1824 NPIV enabled: Override lpfc_sli_mode "
				"parameter (%d) to auto (0).\n",
				lpfc_sli_mode);
			break;
		}
		mode = 2;
		break;
	case 0:
	case 3:
		break;
	default:
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT | LOG_VPORT,
				"1819 Unrecognized lpfc_sli_mode "
				"parameter: %d.\n", lpfc_sli_mode);

		break;
	}

	rc = lpfc_sli_config_port(phba, mode);

	if (rc && lpfc_sli_mode == 3)
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT | LOG_VPORT,
				"1820 Unable to select SLI-3.  "
				"Not supported by adapter.\n");
	if (rc && mode != 2)
		rc = lpfc_sli_config_port(phba, 2);
	if (rc)
		goto lpfc_sli_hba_setup_error;

	/* Enable PCIe device Advanced Error Reporting (AER) if configured */
	if (phba->cfg_aer_support == 1 && !(phba->hba_flag & HBA_AER_ENABLED)) {
		rc = pci_enable_pcie_error_reporting(phba->pcidev);
		if (!rc) {
			lpfc_printf_log(phba, KERN_INFO, LOG_INIT,
					"2709 This device supports "
					"Advanced Error Reporting (AER)\n");
			spin_lock_irq(&phba->hbalock);
			phba->hba_flag |= HBA_AER_ENABLED;
			spin_unlock_irq(&phba->hbalock);
		} else {
			lpfc_printf_log(phba, KERN_INFO, LOG_INIT,
					"2708 This device does not support "
					"Advanced Error Reporting (AER)\n");
			phba->cfg_aer_support = 0;
		}
	}

	if (phba->sli_rev == 3) {
		phba->iocb_cmd_size = SLI3_IOCB_CMD_SIZE;
		phba->iocb_rsp_size = SLI3_IOCB_RSP_SIZE;
	} else {
		phba->iocb_cmd_size = SLI2_IOCB_CMD_SIZE;
		phba->iocb_rsp_size = SLI2_IOCB_RSP_SIZE;
		phba->sli3_options = 0;
	}

	lpfc_printf_log(phba, KERN_INFO, LOG_INIT,
			"0444 Firmware in SLI %x mode. Max_vpi %d\n",
			phba->sli_rev, phba->max_vpi);
	rc = lpfc_sli_ring_map(phba);

	if (rc)
		goto lpfc_sli_hba_setup_error;

	/* Init HBQs */
	if (phba->sli3_options & LPFC_SLI3_HBQ_ENABLED) {
		rc = lpfc_sli_hbq_setup(phba);
		if (rc)
			goto lpfc_sli_hba_setup_error;
	}
	spin_lock_irq(&phba->hbalock);
	phba->sli.sli_flag |= LPFC_PROCESS_LA;
	spin_unlock_irq(&phba->hbalock);

	rc = lpfc_config_port_post(phba);
	if (rc)
		goto lpfc_sli_hba_setup_error;

	return rc;

lpfc_sli_hba_setup_error:
	phba->link_state = LPFC_HBA_ERROR;
	lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
			"0445 Firmware initialization failed\n");
	return rc;
}

static int
lpfc_sli4_read_fcoe_params(struct lpfc_hba *phba,
		LPFC_MBOXQ_t *mboxq)
{
	struct lpfc_dmabuf *mp;
	struct lpfc_mqe *mqe;
	uint32_t data_length;
	int rc;

	/* Program the default value of vlan_id and fc_map */
	phba->valid_vlan = 0;
	phba->fc_map[0] = LPFC_FCOE_FCF_MAP0;
	phba->fc_map[1] = LPFC_FCOE_FCF_MAP1;
	phba->fc_map[2] = LPFC_FCOE_FCF_MAP2;

	mqe = &mboxq->u.mqe;
	if (lpfc_dump_fcoe_param(phba, mboxq))
		return -ENOMEM;

	mp = (struct lpfc_dmabuf *) mboxq->context1;
	rc = lpfc_sli_issue_mbox(phba, mboxq, MBX_POLL);

	lpfc_printf_log(phba, KERN_INFO, LOG_MBOX | LOG_SLI,
			"(%d):2571 Mailbox cmd x%x Status x%x "
			"Data: x%x x%x x%x x%x x%x x%x x%x x%x x%x "
			"x%x x%x x%x x%x x%x x%x x%x x%x x%x "
			"CQ: x%x x%x x%x x%x\n",
			mboxq->vport ? mboxq->vport->vpi : 0,
			bf_get(lpfc_mqe_command, mqe),
			bf_get(lpfc_mqe_status, mqe),
			mqe->un.mb_words[0], mqe->un.mb_words[1],
			mqe->un.mb_words[2], mqe->un.mb_words[3],
			mqe->un.mb_words[4], mqe->un.mb_words[5],
			mqe->un.mb_words[6], mqe->un.mb_words[7],
			mqe->un.mb_words[8], mqe->un.mb_words[9],
			mqe->un.mb_words[10], mqe->un.mb_words[11],
			mqe->un.mb_words[12], mqe->un.mb_words[13],
			mqe->un.mb_words[14], mqe->un.mb_words[15],
			mqe->un.mb_words[16], mqe->un.mb_words[50],
			mboxq->mcqe.word0,
			mboxq->mcqe.mcqe_tag0, 	mboxq->mcqe.mcqe_tag1,
			mboxq->mcqe.trailer);

	if (rc) {
		lpfc_mbuf_free(phba, mp->virt, mp->phys);
		kfree(mp);
		return -EIO;
	}
	data_length = mqe->un.mb_words[5];
	if (data_length > DMP_RGN23_SIZE) {
		lpfc_mbuf_free(phba, mp->virt, mp->phys);
		kfree(mp);
		return -EIO;
	}

	lpfc_parse_fcoe_conf(phba, mp->virt, data_length);
	lpfc_mbuf_free(phba, mp->virt, mp->phys);
	kfree(mp);
	return 0;
}

static int
lpfc_sli4_read_rev(struct lpfc_hba *phba, LPFC_MBOXQ_t *mboxq,
		    uint8_t *vpd, uint32_t *vpd_size)
{
	int rc = 0;
	uint32_t dma_size;
	struct lpfc_dmabuf *dmabuf;
	struct lpfc_mqe *mqe;

	dmabuf = kzalloc(sizeof(struct lpfc_dmabuf), GFP_KERNEL);
	if (!dmabuf)
		return -ENOMEM;

	/*
	 * Get a DMA buffer for the vpd data resulting from the READ_REV
	 * mailbox command.
	 */
	dma_size = *vpd_size;
	dmabuf->virt = dma_alloc_coherent(&phba->pcidev->dev,
					  dma_size,
					  &dmabuf->phys,
					  GFP_KERNEL);
	if (!dmabuf->virt) {
		kfree(dmabuf);
		return -ENOMEM;
	}
	memset(dmabuf->virt, 0, dma_size);

	/*
	 * The SLI4 implementation of READ_REV conflicts at word1,
	 * bits 31:16 and SLI4 adds vpd functionality not present
	 * in SLI3.  This code corrects the conflicts.
	 */
	lpfc_read_rev(phba, mboxq);
	mqe = &mboxq->u.mqe;
	mqe->un.read_rev.vpd_paddr_high = putPaddrHigh(dmabuf->phys);
	mqe->un.read_rev.vpd_paddr_low = putPaddrLow(dmabuf->phys);
	mqe->un.read_rev.word1 &= 0x0000FFFF;
	bf_set(lpfc_mbx_rd_rev_vpd, &mqe->un.read_rev, 1);
	bf_set(lpfc_mbx_rd_rev_avail_len, &mqe->un.read_rev, dma_size);

	rc = lpfc_sli_issue_mbox(phba, mboxq, MBX_POLL);
	if (rc) {
		dma_free_coherent(&phba->pcidev->dev, dma_size,
				  dmabuf->virt, dmabuf->phys);
		kfree(dmabuf);
		return -EIO;
	}

	/*
	 * The available vpd length cannot be bigger than the
	 * DMA buffer passed to the port.  Catch the less than
	 * case and update the caller's size.
	 */
	if (mqe->un.read_rev.avail_vpd_len < *vpd_size)
		*vpd_size = mqe->un.read_rev.avail_vpd_len;

	lpfc_sli_pcimem_bcopy(dmabuf->virt, vpd, *vpd_size);
	dma_free_coherent(&phba->pcidev->dev, dma_size,
			  dmabuf->virt, dmabuf->phys);
	kfree(dmabuf);
	return 0;
}

static void
lpfc_sli4_arm_cqeq_intr(struct lpfc_hba *phba)
{
	uint8_t fcp_eqidx;

	lpfc_sli4_cq_release(phba->sli4_hba.mbx_cq, LPFC_QUEUE_REARM);
	lpfc_sli4_cq_release(phba->sli4_hba.els_cq, LPFC_QUEUE_REARM);
	for (fcp_eqidx = 0; fcp_eqidx < phba->cfg_fcp_eq_count; fcp_eqidx++)
		lpfc_sli4_cq_release(phba->sli4_hba.fcp_cq[fcp_eqidx],
				     LPFC_QUEUE_REARM);
	lpfc_sli4_eq_release(phba->sli4_hba.sp_eq, LPFC_QUEUE_REARM);
	for (fcp_eqidx = 0; fcp_eqidx < phba->cfg_fcp_eq_count; fcp_eqidx++)
		lpfc_sli4_eq_release(phba->sli4_hba.fp_eq[fcp_eqidx],
				     LPFC_QUEUE_REARM);
}

int
lpfc_sli4_hba_setup(struct lpfc_hba *phba)
{
	int rc;
	LPFC_MBOXQ_t *mboxq;
	struct lpfc_mqe *mqe;
	uint8_t *vpd;
	uint32_t vpd_size;
	uint32_t ftr_rsp = 0;
	struct Scsi_Host *shost = lpfc_shost_from_vport(phba->pport);
	struct lpfc_vport *vport = phba->pport;
	struct lpfc_dmabuf *mp;

	/* Perform a PCI function reset to start from clean */
	rc = lpfc_pci_function_reset(phba);
	if (unlikely(rc))
		return -ENODEV;

	/* Check the HBA Host Status Register for readyness */
	rc = lpfc_sli4_post_status_check(phba);
	if (unlikely(rc))
		return -ENODEV;
	else {
		spin_lock_irq(&phba->hbalock);
		phba->sli.sli_flag |= LPFC_SLI_ACTIVE;
		spin_unlock_irq(&phba->hbalock);
	}

	/*
	 * Allocate a single mailbox container for initializing the
	 * port.
	 */
	mboxq = (LPFC_MBOXQ_t *) mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mboxq)
		return -ENOMEM;

	/*
	 * Continue initialization with default values even if driver failed
	 * to read FCoE param config regions
	 */
	if (lpfc_sli4_read_fcoe_params(phba, mboxq))
		lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_INIT,
			"2570 Failed to read FCoE parameters\n");

	/* Issue READ_REV to collect vpd and FW information. */
	vpd_size = SLI4_PAGE_SIZE;
	vpd = kzalloc(vpd_size, GFP_KERNEL);
	if (!vpd) {
		rc = -ENOMEM;
		goto out_free_mbox;
	}

	rc = lpfc_sli4_read_rev(phba, mboxq, vpd, &vpd_size);
	if (unlikely(rc))
		goto out_free_vpd;

	mqe = &mboxq->u.mqe;
	phba->sli_rev = bf_get(lpfc_mbx_rd_rev_sli_lvl, &mqe->un.read_rev);
	if (bf_get(lpfc_mbx_rd_rev_fcoe, &mqe->un.read_rev))
		phba->hba_flag |= HBA_FCOE_SUPPORT;

	if (bf_get(lpfc_mbx_rd_rev_cee_ver, &mqe->un.read_rev) ==
		LPFC_DCBX_CEE_MODE)
		phba->hba_flag |= HBA_FIP_SUPPORT;
	else
		phba->hba_flag &= ~HBA_FIP_SUPPORT;

	if (phba->sli_rev != LPFC_SLI_REV4 ||
	    !(phba->hba_flag & HBA_FCOE_SUPPORT)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
			"0376 READ_REV Error. SLI Level %d "
			"FCoE enabled %d\n",
			phba->sli_rev, phba->hba_flag & HBA_FCOE_SUPPORT);
		rc = -EIO;
		goto out_free_vpd;
	}
	/*
	 * Evaluate the read rev and vpd data. Populate the driver
	 * state with the results. If this routine fails, the failure
	 * is not fatal as the driver will use generic values.
	 */
	rc = lpfc_parse_vpd(phba, vpd, vpd_size);
	if (unlikely(!rc)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
				"0377 Error %d parsing vpd. "
				"Using defaults.\n", rc);
		rc = 0;
	}

	/* Save information as VPD data */
	phba->vpd.rev.biuRev = mqe->un.read_rev.first_hw_rev;
	phba->vpd.rev.smRev = mqe->un.read_rev.second_hw_rev;
	phba->vpd.rev.endecRev = mqe->un.read_rev.third_hw_rev;
	phba->vpd.rev.fcphHigh = bf_get(lpfc_mbx_rd_rev_fcph_high,
					 &mqe->un.read_rev);
	phba->vpd.rev.fcphLow = bf_get(lpfc_mbx_rd_rev_fcph_low,
				       &mqe->un.read_rev);
	phba->vpd.rev.feaLevelHigh = bf_get(lpfc_mbx_rd_rev_ftr_lvl_high,
					    &mqe->un.read_rev);
	phba->vpd.rev.feaLevelLow = bf_get(lpfc_mbx_rd_rev_ftr_lvl_low,
					   &mqe->un.read_rev);
	phba->vpd.rev.sli1FwRev = mqe->un.read_rev.fw_id_rev;
	memcpy(phba->vpd.rev.sli1FwName, mqe->un.read_rev.fw_name, 16);
	phba->vpd.rev.sli2FwRev = mqe->un.read_rev.ulp_fw_id_rev;
	memcpy(phba->vpd.rev.sli2FwName, mqe->un.read_rev.ulp_fw_name, 16);
	phba->vpd.rev.opFwRev = mqe->un.read_rev.fw_id_rev;
	memcpy(phba->vpd.rev.opFwName, mqe->un.read_rev.fw_name, 16);
	lpfc_printf_log(phba, KERN_INFO, LOG_MBOX | LOG_SLI,
			"(%d):0380 READ_REV Status x%x "
			"fw_rev:%s fcphHi:%x fcphLo:%x flHi:%x flLo:%x\n",
			mboxq->vport ? mboxq->vport->vpi : 0,
			bf_get(lpfc_mqe_status, mqe),
			phba->vpd.rev.opFwName,
			phba->vpd.rev.fcphHigh, phba->vpd.rev.fcphLow,
			phba->vpd.rev.feaLevelHigh, phba->vpd.rev.feaLevelLow);

	/*
	 * Discover the port's supported feature set and match it against the
	 * hosts requests.
	 */
	lpfc_request_features(phba, mboxq);
	rc = lpfc_sli_issue_mbox(phba, mboxq, MBX_POLL);
	if (unlikely(rc)) {
		rc = -EIO;
		goto out_free_vpd;
	}

	/*
	 * The port must support FCP initiator mode as this is the
	 * only mode running in the host.
	 */
	if (!(bf_get(lpfc_mbx_rq_ftr_rsp_fcpi, &mqe->un.req_ftrs))) {
		lpfc_printf_log(phba, KERN_WARNING, LOG_MBOX | LOG_SLI,
				"0378 No support for fcpi mode.\n");
		ftr_rsp++;
	}

	/*
	 * If the port cannot support the host's requested features
	 * then turn off the global config parameters to disable the
	 * feature in the driver.  This is not a fatal error.
	 */
	if ((phba->cfg_enable_bg) &&
	    !(bf_get(lpfc_mbx_rq_ftr_rsp_dif, &mqe->un.req_ftrs)))
		ftr_rsp++;

	if (phba->max_vpi && phba->cfg_enable_npiv &&
	    !(bf_get(lpfc_mbx_rq_ftr_rsp_npiv, &mqe->un.req_ftrs)))
		ftr_rsp++;

	if (ftr_rsp) {
		lpfc_printf_log(phba, KERN_WARNING, LOG_MBOX | LOG_SLI,
				"0379 Feature Mismatch Data: x%08x %08x "
				"x%x x%x x%x\n", mqe->un.req_ftrs.word2,
				mqe->un.req_ftrs.word3, phba->cfg_enable_bg,
				phba->cfg_enable_npiv, phba->max_vpi);
		if (!(bf_get(lpfc_mbx_rq_ftr_rsp_dif, &mqe->un.req_ftrs)))
			phba->cfg_enable_bg = 0;
		if (!(bf_get(lpfc_mbx_rq_ftr_rsp_npiv, &mqe->un.req_ftrs)))
			phba->cfg_enable_npiv = 0;
	}

	/* These SLI3 features are assumed in SLI4 */
	spin_lock_irq(&phba->hbalock);
	phba->sli3_options |= (LPFC_SLI3_NPIV_ENABLED | LPFC_SLI3_HBQ_ENABLED);
	spin_unlock_irq(&phba->hbalock);

	/* Read the port's service parameters. */
	rc = lpfc_read_sparam(phba, mboxq, vport->vpi);
	if (rc) {
		phba->link_state = LPFC_HBA_ERROR;
		rc = -ENOMEM;
		goto out_free_vpd;
	}

	mboxq->vport = vport;
	rc = lpfc_sli_issue_mbox(phba, mboxq, MBX_POLL);
	mp = (struct lpfc_dmabuf *) mboxq->context1;
	if (rc == MBX_SUCCESS) {
		memcpy(&vport->fc_sparam, mp->virt, sizeof(struct serv_parm));
		rc = 0;
	}

	/*
	 * This memory was allocated by the lpfc_read_sparam routine. Release
	 * it to the mbuf pool.
	 */
	lpfc_mbuf_free(phba, mp->virt, mp->phys);
	kfree(mp);
	mboxq->context1 = NULL;
	if (unlikely(rc)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
				"0382 READ_SPARAM command failed "
				"status %d, mbxStatus x%x\n",
				rc, bf_get(lpfc_mqe_status, mqe));
		phba->link_state = LPFC_HBA_ERROR;
		rc = -EIO;
		goto out_free_vpd;
	}

	if (phba->cfg_soft_wwnn)
		u64_to_wwn(phba->cfg_soft_wwnn,
			   vport->fc_sparam.nodeName.u.wwn);
	if (phba->cfg_soft_wwpn)
		u64_to_wwn(phba->cfg_soft_wwpn,
			   vport->fc_sparam.portName.u.wwn);
	memcpy(&vport->fc_nodename, &vport->fc_sparam.nodeName,
	       sizeof(struct lpfc_name));
	memcpy(&vport->fc_portname, &vport->fc_sparam.portName,
	       sizeof(struct lpfc_name));

	/* Update the fc_host data structures with new wwn. */
	fc_host_node_name(shost) = wwn_to_u64(vport->fc_nodename.u.wwn);
	fc_host_port_name(shost) = wwn_to_u64(vport->fc_portname.u.wwn);

	/* Register SGL pool to the device using non-embedded mailbox command */
	rc = lpfc_sli4_post_sgl_list(phba);
	if (unlikely(rc)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
				"0582 Error %d during sgl post operation\n",
					rc);
		rc = -ENODEV;
		goto out_free_vpd;
	}

	/* Register SCSI SGL pool to the device */
	rc = lpfc_sli4_repost_scsi_sgl_list(phba);
	if (unlikely(rc)) {
		lpfc_printf_log(phba, KERN_WARNING, LOG_MBOX | LOG_SLI,
				"0383 Error %d during scsi sgl post "
				"operation\n", rc);
		/* Some Scsi buffers were moved to the abort scsi list */
		/* A pci function reset will repost them */
		rc = -ENODEV;
		goto out_free_vpd;
	}

	/* Post the rpi header region to the device. */
	rc = lpfc_sli4_post_all_rpi_hdrs(phba);
	if (unlikely(rc)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
				"0393 Error %d during rpi post operation\n",
				rc);
		rc = -ENODEV;
		goto out_free_vpd;
	}

	/* Set up all the queues to the device */
	rc = lpfc_sli4_queue_setup(phba);
	if (unlikely(rc)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
				"0381 Error %d during queue setup.\n ", rc);
		goto out_stop_timers;
	}

	/* Arm the CQs and then EQs on device */
	lpfc_sli4_arm_cqeq_intr(phba);

	/* Indicate device interrupt mode */
	phba->sli4_hba.intr_enable = 1;

	/* Allow asynchronous mailbox command to go through */
	spin_lock_irq(&phba->hbalock);
	phba->sli.sli_flag &= ~LPFC_SLI_ASYNC_MBX_BLK;
	spin_unlock_irq(&phba->hbalock);

	/* Post receive buffers to the device */
	lpfc_sli4_rb_setup(phba);

	/* Reset HBA FCF states after HBA reset */
	phba->fcf.fcf_flag = 0;
	phba->fcf.current_rec.flag = 0;

	/* Start the ELS watchdog timer */
	mod_timer(&vport->els_tmofunc,
		  jiffies + HZ * (phba->fc_ratov * 2));

	/* Start heart beat timer */
	mod_timer(&phba->hb_tmofunc,
		  jiffies + HZ * LPFC_HB_MBOX_INTERVAL);
	phba->hb_outstanding = 0;
	phba->last_completion_time = jiffies;

	/* Start error attention (ERATT) polling timer */
	mod_timer(&phba->eratt_poll, jiffies + HZ * LPFC_ERATT_POLL_INTERVAL);

	/*
	 * The port is ready, set the host's link state to LINK_DOWN
	 * in preparation for link interrupts.
	 */
	lpfc_init_link(phba, mboxq, phba->cfg_topology, phba->cfg_link_speed);
	mboxq->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
	lpfc_set_loopback_flag(phba);
	/* Change driver state to LPFC_LINK_DOWN right before init link */
	spin_lock_irq(&phba->hbalock);
	phba->link_state = LPFC_LINK_DOWN;
	spin_unlock_irq(&phba->hbalock);
	rc = lpfc_sli_issue_mbox(phba, mboxq, MBX_NOWAIT);
	if (unlikely(rc != MBX_NOT_FINISHED)) {
		kfree(vpd);
		return 0;
	} else
		rc = -EIO;

	/* Unset all the queues set up in this routine when error out */
	if (rc)
		lpfc_sli4_queue_unset(phba);

out_stop_timers:
	if (rc)
		lpfc_stop_hba_timers(phba);
out_free_vpd:
	kfree(vpd);
out_free_mbox:
	mempool_free(mboxq, phba->mbox_mem_pool);
	return rc;
}

void
lpfc_mbox_timeout(unsigned long ptr)
{
	struct lpfc_hba  *phba = (struct lpfc_hba *) ptr;
	unsigned long iflag;
	uint32_t tmo_posted;

	spin_lock_irqsave(&phba->pport->work_port_lock, iflag);
	tmo_posted = phba->pport->work_port_events & WORKER_MBOX_TMO;
	if (!tmo_posted)
		phba->pport->work_port_events |= WORKER_MBOX_TMO;
	spin_unlock_irqrestore(&phba->pport->work_port_lock, iflag);

	if (!tmo_posted)
		lpfc_worker_wake_up(phba);
	return;
}


void
lpfc_mbox_timeout_handler(struct lpfc_hba *phba)
{
	LPFC_MBOXQ_t *pmbox = phba->sli.mbox_active;
	MAILBOX_t *mb = &pmbox->u.mb;
	struct lpfc_sli *psli = &phba->sli;
	struct lpfc_sli_ring *pring;

	/* Check the pmbox pointer first.  There is a race condition
	 * between the mbox timeout handler getting executed in the
	 * worklist and the mailbox actually completing. When this
	 * race condition occurs, the mbox_active will be NULL.
	 */
	spin_lock_irq(&phba->hbalock);
	if (pmbox == NULL) {
		lpfc_printf_log(phba, KERN_WARNING,
				LOG_MBOX | LOG_SLI,
				"0353 Active Mailbox cleared - mailbox timeout "
				"exiting\n");
		spin_unlock_irq(&phba->hbalock);
		return;
	}

	/* Mbox cmd <mbxCommand> timeout */
	lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
			"0310 Mailbox command x%x timeout Data: x%x x%x x%p\n",
			mb->mbxCommand,
			phba->pport->port_state,
			phba->sli.sli_flag,
			phba->sli.mbox_active);
	spin_unlock_irq(&phba->hbalock);

	/* Setting state unknown so lpfc_sli_abort_iocb_ring
	 * would get IOCB_ERROR from lpfc_sli_issue_iocb, allowing
	 * it to fail all oustanding SCSI IO.
	 */
	spin_lock_irq(&phba->pport->work_port_lock);
	phba->pport->work_port_events &= ~WORKER_MBOX_TMO;
	spin_unlock_irq(&phba->pport->work_port_lock);
	spin_lock_irq(&phba->hbalock);
	phba->link_state = LPFC_LINK_UNKNOWN;
	psli->sli_flag &= ~LPFC_SLI_ACTIVE;
	spin_unlock_irq(&phba->hbalock);

	pring = &psli->ring[psli->fcp_ring];
	lpfc_sli_abort_iocb_ring(phba, pring);

	lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
			"0345 Resetting board due to mailbox timeout\n");

	/* Reset the HBA device */
	lpfc_reset_hba(phba);
}

static int
lpfc_sli_issue_mbox_s3(struct lpfc_hba *phba, LPFC_MBOXQ_t *pmbox,
		       uint32_t flag)
{
	MAILBOX_t *mb;
	struct lpfc_sli *psli = &phba->sli;
	uint32_t status, evtctr;
	uint32_t ha_copy;
	int i;
	unsigned long timeout;
	unsigned long drvr_flag = 0;
	uint32_t word0, ldata;
	void __iomem *to_slim;
	int processing_queue = 0;

	spin_lock_irqsave(&phba->hbalock, drvr_flag);
	if (!pmbox) {
		phba->sli.sli_flag &= ~LPFC_SLI_MBOX_ACTIVE;
		/* processing mbox queue from intr_handler */
		if (unlikely(psli->sli_flag & LPFC_SLI_ASYNC_MBX_BLK)) {
			spin_unlock_irqrestore(&phba->hbalock, drvr_flag);
			return MBX_SUCCESS;
		}
		processing_queue = 1;
		pmbox = lpfc_mbox_get(phba);
		if (!pmbox) {
			spin_unlock_irqrestore(&phba->hbalock, drvr_flag);
			return MBX_SUCCESS;
		}
	}

	if (pmbox->mbox_cmpl && pmbox->mbox_cmpl != lpfc_sli_def_mbox_cmpl &&
		pmbox->mbox_cmpl != lpfc_sli_wake_mbox_wait) {
		if(!pmbox->vport) {
			spin_unlock_irqrestore(&phba->hbalock, drvr_flag);
			lpfc_printf_log(phba, KERN_ERR,
					LOG_MBOX | LOG_VPORT,
					"1806 Mbox x%x failed. No vport\n",
					pmbox->u.mb.mbxCommand);
			dump_stack();
			goto out_not_finished;
		}
	}

	/* If the PCI channel is in offline state, do not post mbox. */
	if (unlikely(pci_channel_offline(phba->pcidev))) {
		spin_unlock_irqrestore(&phba->hbalock, drvr_flag);
		goto out_not_finished;
	}

	/* If HBA has a deferred error attention, fail the iocb. */
	if (unlikely(phba->hba_flag & DEFER_ERATT)) {
		spin_unlock_irqrestore(&phba->hbalock, drvr_flag);
		goto out_not_finished;
	}

	psli = &phba->sli;

	mb = &pmbox->u.mb;
	status = MBX_SUCCESS;

	if (phba->link_state == LPFC_HBA_ERROR) {
		spin_unlock_irqrestore(&phba->hbalock, drvr_flag);

		/* Mbox command <mbxCommand> cannot issue */
		lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
				"(%d):0311 Mailbox command x%x cannot "
				"issue Data: x%x x%x\n",
				pmbox->vport ? pmbox->vport->vpi : 0,
				pmbox->u.mb.mbxCommand, psli->sli_flag, flag);
		goto out_not_finished;
	}

	if (mb->mbxCommand != MBX_KILL_BOARD && flag & MBX_NOWAIT &&
	    !(readl(phba->HCregaddr) & HC_MBINT_ENA)) {
		spin_unlock_irqrestore(&phba->hbalock, drvr_flag);
		lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
				"(%d):2528 Mailbox command x%x cannot "
				"issue Data: x%x x%x\n",
				pmbox->vport ? pmbox->vport->vpi : 0,
				pmbox->u.mb.mbxCommand, psli->sli_flag, flag);
		goto out_not_finished;
	}

	if (psli->sli_flag & LPFC_SLI_MBOX_ACTIVE) {
		/* Polling for a mbox command when another one is already active
		 * is not allowed in SLI. Also, the driver must have established
		 * SLI2 mode to queue and process multiple mbox commands.
		 */

		if (flag & MBX_POLL) {
			spin_unlock_irqrestore(&phba->hbalock, drvr_flag);

			/* Mbox command <mbxCommand> cannot issue */
			lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
					"(%d):2529 Mailbox command x%x "
					"cannot issue Data: x%x x%x\n",
					pmbox->vport ? pmbox->vport->vpi : 0,
					pmbox->u.mb.mbxCommand,
					psli->sli_flag, flag);
			goto out_not_finished;
		}

		if (!(psli->sli_flag & LPFC_SLI_ACTIVE)) {
			spin_unlock_irqrestore(&phba->hbalock, drvr_flag);
			/* Mbox command <mbxCommand> cannot issue */
			lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
					"(%d):2530 Mailbox command x%x "
					"cannot issue Data: x%x x%x\n",
					pmbox->vport ? pmbox->vport->vpi : 0,
					pmbox->u.mb.mbxCommand,
					psli->sli_flag, flag);
			goto out_not_finished;
		}

		/* Another mailbox command is still being processed, queue this
		 * command to be processed later.
		 */
		lpfc_mbox_put(phba, pmbox);

		/* Mbox cmd issue - BUSY */
		lpfc_printf_log(phba, KERN_INFO, LOG_MBOX | LOG_SLI,
				"(%d):0308 Mbox cmd issue - BUSY Data: "
				"x%x x%x x%x x%x\n",
				pmbox->vport ? pmbox->vport->vpi : 0xffffff,
				mb->mbxCommand, phba->pport->port_state,
				psli->sli_flag, flag);

		psli->slistat.mbox_busy++;
		spin_unlock_irqrestore(&phba->hbalock, drvr_flag);

		if (pmbox->vport) {
			lpfc_debugfs_disc_trc(pmbox->vport,
				LPFC_DISC_TRC_MBOX_VPORT,
				"MBOX Bsy vport:  cmd:x%x mb:x%x x%x",
				(uint32_t)mb->mbxCommand,
				mb->un.varWords[0], mb->un.varWords[1]);
		}
		else {
			lpfc_debugfs_disc_trc(phba->pport,
				LPFC_DISC_TRC_MBOX,
				"MBOX Bsy:        cmd:x%x mb:x%x x%x",
				(uint32_t)mb->mbxCommand,
				mb->un.varWords[0], mb->un.varWords[1]);
		}

		return MBX_BUSY;
	}

	psli->sli_flag |= LPFC_SLI_MBOX_ACTIVE;

	/* If we are not polling, we MUST be in SLI2 mode */
	if (flag != MBX_POLL) {
		if (!(psli->sli_flag & LPFC_SLI_ACTIVE) &&
		    (mb->mbxCommand != MBX_KILL_BOARD)) {
			psli->sli_flag &= ~LPFC_SLI_MBOX_ACTIVE;
			spin_unlock_irqrestore(&phba->hbalock, drvr_flag);
			/* Mbox command <mbxCommand> cannot issue */
			lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
					"(%d):2531 Mailbox command x%x "
					"cannot issue Data: x%x x%x\n",
					pmbox->vport ? pmbox->vport->vpi : 0,
					pmbox->u.mb.mbxCommand,
					psli->sli_flag, flag);
			goto out_not_finished;
		}
		/* timeout active mbox command */
		mod_timer(&psli->mbox_tmo, (jiffies +
			       (HZ * lpfc_mbox_tmo_val(phba, mb->mbxCommand))));
	}

	/* Mailbox cmd <cmd> issue */
	lpfc_printf_log(phba, KERN_INFO, LOG_MBOX | LOG_SLI,
			"(%d):0309 Mailbox cmd x%x issue Data: x%x x%x "
			"x%x\n",
			pmbox->vport ? pmbox->vport->vpi : 0,
			mb->mbxCommand, phba->pport->port_state,
			psli->sli_flag, flag);

	if (mb->mbxCommand != MBX_HEARTBEAT) {
		if (pmbox->vport) {
			lpfc_debugfs_disc_trc(pmbox->vport,
				LPFC_DISC_TRC_MBOX_VPORT,
				"MBOX Send vport: cmd:x%x mb:x%x x%x",
				(uint32_t)mb->mbxCommand,
				mb->un.varWords[0], mb->un.varWords[1]);
		}
		else {
			lpfc_debugfs_disc_trc(phba->pport,
				LPFC_DISC_TRC_MBOX,
				"MBOX Send:       cmd:x%x mb:x%x x%x",
				(uint32_t)mb->mbxCommand,
				mb->un.varWords[0], mb->un.varWords[1]);
		}
	}

	psli->slistat.mbox_cmd++;
	evtctr = psli->slistat.mbox_event;

	/* next set own bit for the adapter and copy over command word */
	mb->mbxOwner = OWN_CHIP;

	if (psli->sli_flag & LPFC_SLI_ACTIVE) {
		/* Populate mbox extension offset word. */
		if (pmbox->in_ext_byte_len || pmbox->out_ext_byte_len) {
			*(((uint32_t *)mb) + pmbox->mbox_offset_word)
				= (uint8_t *)phba->mbox_ext
				  - (uint8_t *)phba->mbox;
		}

		/* Copy the mailbox extension data */
		if (pmbox->in_ext_byte_len && pmbox->context2) {
			lpfc_sli_pcimem_bcopy(pmbox->context2,
				(uint8_t *)phba->mbox_ext,
				pmbox->in_ext_byte_len);
		}
		/* Copy command data to host SLIM area */
		lpfc_sli_pcimem_bcopy(mb, phba->mbox, MAILBOX_CMD_SIZE);
	} else {
		/* Populate mbox extension offset word. */
		if (pmbox->in_ext_byte_len || pmbox->out_ext_byte_len)
			*(((uint32_t *)mb) + pmbox->mbox_offset_word)
				= MAILBOX_HBA_EXT_OFFSET;

		/* Copy the mailbox extension data */
		if (pmbox->in_ext_byte_len && pmbox->context2) {
			lpfc_memcpy_to_slim(phba->MBslimaddr +
				MAILBOX_HBA_EXT_OFFSET,
				pmbox->context2, pmbox->in_ext_byte_len);

		}
		if (mb->mbxCommand == MBX_CONFIG_PORT) {
			/* copy command data into host mbox for cmpl */
			lpfc_sli_pcimem_bcopy(mb, phba->mbox, MAILBOX_CMD_SIZE);
		}

		/* First copy mbox command data to HBA SLIM, skip past first
		   word */
		to_slim = phba->MBslimaddr + sizeof (uint32_t);
		lpfc_memcpy_to_slim(to_slim, &mb->un.varWords[0],
			    MAILBOX_CMD_SIZE - sizeof (uint32_t));

		/* Next copy over first word, with mbxOwner set */
		ldata = *((uint32_t *)mb);
		to_slim = phba->MBslimaddr;
		writel(ldata, to_slim);
		readl(to_slim); /* flush */

		if (mb->mbxCommand == MBX_CONFIG_PORT) {
			/* switch over to host mailbox */
			psli->sli_flag |= LPFC_SLI_ACTIVE;
		}
	}

	wmb();

	switch (flag) {
	case MBX_NOWAIT:
		/* Set up reference to mailbox command */
		psli->mbox_active = pmbox;
		/* Interrupt board to do it */
		writel(CA_MBATT, phba->CAregaddr);
		readl(phba->CAregaddr); /* flush */
		/* Don't wait for it to finish, just return */
		break;

	case MBX_POLL:
		/* Set up null reference to mailbox command */
		psli->mbox_active = NULL;
		/* Interrupt board to do it */
		writel(CA_MBATT, phba->CAregaddr);
		readl(phba->CAregaddr); /* flush */

		if (psli->sli_flag & LPFC_SLI_ACTIVE) {
			/* First read mbox status word */
			word0 = *((uint32_t *)phba->mbox);
			word0 = le32_to_cpu(word0);
		} else {
			/* First read mbox status word */
			word0 = readl(phba->MBslimaddr);
		}

		/* Read the HBA Host Attention Register */
		ha_copy = readl(phba->HAregaddr);
		timeout = msecs_to_jiffies(lpfc_mbox_tmo_val(phba,
							     mb->mbxCommand) *
					   1000) + jiffies;
		i = 0;
		/* Wait for command to complete */
		while (((word0 & OWN_CHIP) == OWN_CHIP) ||
		       (!(ha_copy & HA_MBATT) &&
			(phba->link_state > LPFC_WARM_START))) {
			if (time_after(jiffies, timeout)) {
				psli->sli_flag &= ~LPFC_SLI_MBOX_ACTIVE;
				spin_unlock_irqrestore(&phba->hbalock,
						       drvr_flag);
				goto out_not_finished;
			}

			/* Check if we took a mbox interrupt while we were
			   polling */
			if (((word0 & OWN_CHIP) != OWN_CHIP)
			    && (evtctr != psli->slistat.mbox_event))
				break;

			if (i++ > 10) {
				spin_unlock_irqrestore(&phba->hbalock,
						       drvr_flag);
				msleep(1);
				spin_lock_irqsave(&phba->hbalock, drvr_flag);
			}

			if (psli->sli_flag & LPFC_SLI_ACTIVE) {
				/* First copy command data */
				word0 = *((uint32_t *)phba->mbox);
				word0 = le32_to_cpu(word0);
				if (mb->mbxCommand == MBX_CONFIG_PORT) {
					MAILBOX_t *slimmb;
					uint32_t slimword0;
					/* Check real SLIM for any errors */
					slimword0 = readl(phba->MBslimaddr);
					slimmb = (MAILBOX_t *) & slimword0;
					if (((slimword0 & OWN_CHIP) != OWN_CHIP)
					    && slimmb->mbxStatus) {
						psli->sli_flag &=
						    ~LPFC_SLI_ACTIVE;
						word0 = slimword0;
					}
				}
			} else {
				/* First copy command data */
				word0 = readl(phba->MBslimaddr);
			}
			/* Read the HBA Host Attention Register */
			ha_copy = readl(phba->HAregaddr);
		}

		if (psli->sli_flag & LPFC_SLI_ACTIVE) {
			/* copy results back to user */
			lpfc_sli_pcimem_bcopy(phba->mbox, mb, MAILBOX_CMD_SIZE);
			/* Copy the mailbox extension data */
			if (pmbox->out_ext_byte_len && pmbox->context2) {
				lpfc_sli_pcimem_bcopy(phba->mbox_ext,
						      pmbox->context2,
						      pmbox->out_ext_byte_len);
			}
		} else {
			/* First copy command data */
			lpfc_memcpy_from_slim(mb, phba->MBslimaddr,
							MAILBOX_CMD_SIZE);
			/* Copy the mailbox extension data */
			if (pmbox->out_ext_byte_len && pmbox->context2) {
				lpfc_memcpy_from_slim(pmbox->context2,
					phba->MBslimaddr +
					MAILBOX_HBA_EXT_OFFSET,
					pmbox->out_ext_byte_len);
			}
		}

		writel(HA_MBATT, phba->HAregaddr);
		readl(phba->HAregaddr); /* flush */

		psli->sli_flag &= ~LPFC_SLI_MBOX_ACTIVE;
		status = mb->mbxStatus;
	}

	spin_unlock_irqrestore(&phba->hbalock, drvr_flag);
	return status;

out_not_finished:
	if (processing_queue) {
		pmbox->u.mb.mbxStatus = MBX_NOT_FINISHED;
		lpfc_mbox_cmpl_put(phba, pmbox);
	}
	return MBX_NOT_FINISHED;
}

static int
lpfc_sli4_async_mbox_block(struct lpfc_hba *phba)
{
	struct lpfc_sli *psli = &phba->sli;
	uint8_t actcmd = MBX_HEARTBEAT;
	int rc = 0;
	unsigned long timeout;

	/* Mark the asynchronous mailbox command posting as blocked */
	spin_lock_irq(&phba->hbalock);
	psli->sli_flag |= LPFC_SLI_ASYNC_MBX_BLK;
	if (phba->sli.mbox_active)
		actcmd = phba->sli.mbox_active->u.mb.mbxCommand;
	spin_unlock_irq(&phba->hbalock);
	/* Determine how long we might wait for the active mailbox
	 * command to be gracefully completed by firmware.
	 */
	timeout = msecs_to_jiffies(lpfc_mbox_tmo_val(phba, actcmd) * 1000) +
				   jiffies;
	/* Wait for the outstnading mailbox command to complete */
	while (phba->sli.mbox_active) {
		/* Check active mailbox complete status every 2ms */
		msleep(2);
		if (time_after(jiffies, timeout)) {
			/* Timeout, marked the outstanding cmd not complete */
			rc = 1;
			break;
		}
	}

	/* Can not cleanly block async mailbox command, fails it */
	if (rc) {
		spin_lock_irq(&phba->hbalock);
		psli->sli_flag &= ~LPFC_SLI_ASYNC_MBX_BLK;
		spin_unlock_irq(&phba->hbalock);
	}
	return rc;
}

static void
lpfc_sli4_async_mbox_unblock(struct lpfc_hba *phba)
{
	struct lpfc_sli *psli = &phba->sli;

	spin_lock_irq(&phba->hbalock);
	if (!(psli->sli_flag & LPFC_SLI_ASYNC_MBX_BLK)) {
		/* Asynchronous mailbox posting is not blocked, do nothing */
		spin_unlock_irq(&phba->hbalock);
		return;
	}

	/* Outstanding synchronous mailbox command is guaranteed to be done,
	 * successful or timeout, after timing-out the outstanding mailbox
	 * command shall always be removed, so just unblock posting async
	 * mailbox command and resume
	 */
	psli->sli_flag &= ~LPFC_SLI_ASYNC_MBX_BLK;
	spin_unlock_irq(&phba->hbalock);

	/* wake up worker thread to post asynchronlous mailbox command */
	lpfc_worker_wake_up(phba);
}

static int
lpfc_sli4_post_sync_mbox(struct lpfc_hba *phba, LPFC_MBOXQ_t *mboxq)
{
	int rc = MBX_SUCCESS;
	unsigned long iflag;
	uint32_t db_ready;
	uint32_t mcqe_status;
	uint32_t mbx_cmnd;
	unsigned long timeout;
	struct lpfc_sli *psli = &phba->sli;
	struct lpfc_mqe *mb = &mboxq->u.mqe;
	struct lpfc_bmbx_create *mbox_rgn;
	struct dma_address *dma_address;
	struct lpfc_register bmbx_reg;

	/*
	 * Only one mailbox can be active to the bootstrap mailbox region
	 * at a time and there is no queueing provided.
	 */
	spin_lock_irqsave(&phba->hbalock, iflag);
	if (psli->sli_flag & LPFC_SLI_MBOX_ACTIVE) {
		spin_unlock_irqrestore(&phba->hbalock, iflag);
		lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
				"(%d):2532 Mailbox command x%x (x%x) "
				"cannot issue Data: x%x x%x\n",
				mboxq->vport ? mboxq->vport->vpi : 0,
				mboxq->u.mb.mbxCommand,
				lpfc_sli4_mbox_opcode_get(phba, mboxq),
				psli->sli_flag, MBX_POLL);
		return MBXERR_ERROR;
	}
	/* The server grabs the token and owns it until release */
	psli->sli_flag |= LPFC_SLI_MBOX_ACTIVE;
	phba->sli.mbox_active = mboxq;
	spin_unlock_irqrestore(&phba->hbalock, iflag);

	/*
	 * Initialize the bootstrap memory region to avoid stale data areas
	 * in the mailbox post.  Then copy the caller's mailbox contents to
	 * the bmbx mailbox region.
	 */
	mbx_cmnd = bf_get(lpfc_mqe_command, mb);
	memset(phba->sli4_hba.bmbx.avirt, 0, sizeof(struct lpfc_bmbx_create));
	lpfc_sli_pcimem_bcopy(mb, phba->sli4_hba.bmbx.avirt,
			      sizeof(struct lpfc_mqe));

	/* Post the high mailbox dma address to the port and wait for ready. */
	dma_address = &phba->sli4_hba.bmbx.dma_address;
	writel(dma_address->addr_hi, phba->sli4_hba.BMBXregaddr);

	timeout = msecs_to_jiffies(lpfc_mbox_tmo_val(phba, mbx_cmnd)
				   * 1000) + jiffies;
	do {
		bmbx_reg.word0 = readl(phba->sli4_hba.BMBXregaddr);
		db_ready = bf_get(lpfc_bmbx_rdy, &bmbx_reg);
		if (!db_ready)
			msleep(2);

		if (time_after(jiffies, timeout)) {
			rc = MBXERR_ERROR;
			goto exit;
		}
	} while (!db_ready);

	/* Post the low mailbox dma address to the port. */
	writel(dma_address->addr_lo, phba->sli4_hba.BMBXregaddr);
	timeout = msecs_to_jiffies(lpfc_mbox_tmo_val(phba, mbx_cmnd)
				   * 1000) + jiffies;
	do {
		bmbx_reg.word0 = readl(phba->sli4_hba.BMBXregaddr);
		db_ready = bf_get(lpfc_bmbx_rdy, &bmbx_reg);
		if (!db_ready)
			msleep(2);

		if (time_after(jiffies, timeout)) {
			rc = MBXERR_ERROR;
			goto exit;
		}
	} while (!db_ready);

	/*
	 * Read the CQ to ensure the mailbox has completed.
	 * If so, update the mailbox status so that the upper layers
	 * can complete the request normally.
	 */
	lpfc_sli_pcimem_bcopy(phba->sli4_hba.bmbx.avirt, mb,
			      sizeof(struct lpfc_mqe));
	mbox_rgn = (struct lpfc_bmbx_create *) phba->sli4_hba.bmbx.avirt;
	lpfc_sli_pcimem_bcopy(&mbox_rgn->mcqe, &mboxq->mcqe,
			      sizeof(struct lpfc_mcqe));
	mcqe_status = bf_get(lpfc_mcqe_status, &mbox_rgn->mcqe);

	/* Prefix the mailbox status with range x4000 to note SLI4 status. */
	if (mcqe_status != MB_CQE_STATUS_SUCCESS) {
		bf_set(lpfc_mqe_status, mb, LPFC_MBX_ERROR_RANGE | mcqe_status);
		rc = MBXERR_ERROR;
	}

	lpfc_printf_log(phba, KERN_INFO, LOG_MBOX | LOG_SLI,
			"(%d):0356 Mailbox cmd x%x (x%x) Status x%x "
			"Data: x%x x%x x%x x%x x%x x%x x%x x%x x%x x%x x%x"
			" x%x x%x CQ: x%x x%x x%x x%x\n",
			mboxq->vport ? mboxq->vport->vpi : 0,
			mbx_cmnd, lpfc_sli4_mbox_opcode_get(phba, mboxq),
			bf_get(lpfc_mqe_status, mb),
			mb->un.mb_words[0], mb->un.mb_words[1],
			mb->un.mb_words[2], mb->un.mb_words[3],
			mb->un.mb_words[4], mb->un.mb_words[5],
			mb->un.mb_words[6], mb->un.mb_words[7],
			mb->un.mb_words[8], mb->un.mb_words[9],
			mb->un.mb_words[10], mb->un.mb_words[11],
			mb->un.mb_words[12], mboxq->mcqe.word0,
			mboxq->mcqe.mcqe_tag0, 	mboxq->mcqe.mcqe_tag1,
			mboxq->mcqe.trailer);
exit:
	/* We are holding the token, no needed for lock when release */
	spin_lock_irqsave(&phba->hbalock, iflag);
	psli->sli_flag &= ~LPFC_SLI_MBOX_ACTIVE;
	phba->sli.mbox_active = NULL;
	spin_unlock_irqrestore(&phba->hbalock, iflag);
	return rc;
}

static int
lpfc_sli_issue_mbox_s4(struct lpfc_hba *phba, LPFC_MBOXQ_t *mboxq,
		       uint32_t flag)
{
	struct lpfc_sli *psli = &phba->sli;
	unsigned long iflags;
	int rc;

	rc = lpfc_mbox_dev_check(phba);
	if (unlikely(rc)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
				"(%d):2544 Mailbox command x%x (x%x) "
				"cannot issue Data: x%x x%x\n",
				mboxq->vport ? mboxq->vport->vpi : 0,
				mboxq->u.mb.mbxCommand,
				lpfc_sli4_mbox_opcode_get(phba, mboxq),
				psli->sli_flag, flag);
		goto out_not_finished;
	}

	/* Detect polling mode and jump to a handler */
	if (!phba->sli4_hba.intr_enable) {
		if (flag == MBX_POLL)
			rc = lpfc_sli4_post_sync_mbox(phba, mboxq);
		else
			rc = -EIO;
		if (rc != MBX_SUCCESS)
			lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
					"(%d):2541 Mailbox command x%x "
					"(x%x) cannot issue Data: x%x x%x\n",
					mboxq->vport ? mboxq->vport->vpi : 0,
					mboxq->u.mb.mbxCommand,
					lpfc_sli4_mbox_opcode_get(phba, mboxq),
					psli->sli_flag, flag);
		return rc;
	} else if (flag == MBX_POLL) {
		lpfc_printf_log(phba, KERN_WARNING, LOG_MBOX | LOG_SLI,
				"(%d):2542 Try to issue mailbox command "
				"x%x (x%x) synchronously ahead of async"
				"mailbox command queue: x%x x%x\n",
				mboxq->vport ? mboxq->vport->vpi : 0,
				mboxq->u.mb.mbxCommand,
				lpfc_sli4_mbox_opcode_get(phba, mboxq),
				psli->sli_flag, flag);
		/* Try to block the asynchronous mailbox posting */
		rc = lpfc_sli4_async_mbox_block(phba);
		if (!rc) {
			/* Successfully blocked, now issue sync mbox cmd */
			rc = lpfc_sli4_post_sync_mbox(phba, mboxq);
			if (rc != MBX_SUCCESS)
				lpfc_printf_log(phba, KERN_ERR,
						LOG_MBOX | LOG_SLI,
						"(%d):2597 Mailbox command "
						"x%x (x%x) cannot issue "
						"Data: x%x x%x\n",
						mboxq->vport ?
						mboxq->vport->vpi : 0,
						mboxq->u.mb.mbxCommand,
						lpfc_sli4_mbox_opcode_get(phba,
								mboxq),
						psli->sli_flag, flag);
			/* Unblock the async mailbox posting afterward */
			lpfc_sli4_async_mbox_unblock(phba);
		}
		return rc;
	}

	/* Now, interrupt mode asynchrous mailbox command */
	rc = lpfc_mbox_cmd_check(phba, mboxq);
	if (rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
				"(%d):2543 Mailbox command x%x (x%x) "
				"cannot issue Data: x%x x%x\n",
				mboxq->vport ? mboxq->vport->vpi : 0,
				mboxq->u.mb.mbxCommand,
				lpfc_sli4_mbox_opcode_get(phba, mboxq),
				psli->sli_flag, flag);
		goto out_not_finished;
	}

	/* Put the mailbox command to the driver internal FIFO */
	psli->slistat.mbox_busy++;
	spin_lock_irqsave(&phba->hbalock, iflags);
	lpfc_mbox_put(phba, mboxq);
	spin_unlock_irqrestore(&phba->hbalock, iflags);
	lpfc_printf_log(phba, KERN_INFO, LOG_MBOX | LOG_SLI,
			"(%d):0354 Mbox cmd issue - Enqueue Data: "
			"x%x (x%x) x%x x%x x%x\n",
			mboxq->vport ? mboxq->vport->vpi : 0xffffff,
			bf_get(lpfc_mqe_command, &mboxq->u.mqe),
			lpfc_sli4_mbox_opcode_get(phba, mboxq),
			phba->pport->port_state,
			psli->sli_flag, MBX_NOWAIT);
	/* Wake up worker thread to transport mailbox command from head */
	lpfc_worker_wake_up(phba);

	return MBX_BUSY;

out_not_finished:
	return MBX_NOT_FINISHED;
}

int
lpfc_sli4_post_async_mbox(struct lpfc_hba *phba)
{
	struct lpfc_sli *psli = &phba->sli;
	LPFC_MBOXQ_t *mboxq;
	int rc = MBX_SUCCESS;
	unsigned long iflags;
	struct lpfc_mqe *mqe;
	uint32_t mbx_cmnd;

	/* Check interrupt mode before post async mailbox command */
	if (unlikely(!phba->sli4_hba.intr_enable))
		return MBX_NOT_FINISHED;

	/* Check for mailbox command service token */
	spin_lock_irqsave(&phba->hbalock, iflags);
	if (unlikely(psli->sli_flag & LPFC_SLI_ASYNC_MBX_BLK)) {
		spin_unlock_irqrestore(&phba->hbalock, iflags);
		return MBX_NOT_FINISHED;
	}
	if (psli->sli_flag & LPFC_SLI_MBOX_ACTIVE) {
		spin_unlock_irqrestore(&phba->hbalock, iflags);
		return MBX_NOT_FINISHED;
	}
	if (unlikely(phba->sli.mbox_active)) {
		spin_unlock_irqrestore(&phba->hbalock, iflags);
		lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
				"0384 There is pending active mailbox cmd\n");
		return MBX_NOT_FINISHED;
	}
	/* Take the mailbox command service token */
	psli->sli_flag |= LPFC_SLI_MBOX_ACTIVE;

	/* Get the next mailbox command from head of queue */
	mboxq = lpfc_mbox_get(phba);

	/* If no more mailbox command waiting for post, we're done */
	if (!mboxq) {
		psli->sli_flag &= ~LPFC_SLI_MBOX_ACTIVE;
		spin_unlock_irqrestore(&phba->hbalock, iflags);
		return MBX_SUCCESS;
	}
	phba->sli.mbox_active = mboxq;
	spin_unlock_irqrestore(&phba->hbalock, iflags);

	/* Check device readiness for posting mailbox command */
	rc = lpfc_mbox_dev_check(phba);
	if (unlikely(rc))
		/* Driver clean routine will clean up pending mailbox */
		goto out_not_finished;

	/* Prepare the mbox command to be posted */
	mqe = &mboxq->u.mqe;
	mbx_cmnd = bf_get(lpfc_mqe_command, mqe);

	/* Start timer for the mbox_tmo and log some mailbox post messages */
	mod_timer(&psli->mbox_tmo, (jiffies +
		  (HZ * lpfc_mbox_tmo_val(phba, mbx_cmnd))));

	lpfc_printf_log(phba, KERN_INFO, LOG_MBOX | LOG_SLI,
			"(%d):0355 Mailbox cmd x%x (x%x) issue Data: "
			"x%x x%x\n",
			mboxq->vport ? mboxq->vport->vpi : 0, mbx_cmnd,
			lpfc_sli4_mbox_opcode_get(phba, mboxq),
			phba->pport->port_state, psli->sli_flag);

	if (mbx_cmnd != MBX_HEARTBEAT) {
		if (mboxq->vport) {
			lpfc_debugfs_disc_trc(mboxq->vport,
				LPFC_DISC_TRC_MBOX_VPORT,
				"MBOX Send vport: cmd:x%x mb:x%x x%x",
				mbx_cmnd, mqe->un.mb_words[0],
				mqe->un.mb_words[1]);
		} else {
			lpfc_debugfs_disc_trc(phba->pport,
				LPFC_DISC_TRC_MBOX,
				"MBOX Send: cmd:x%x mb:x%x x%x",
				mbx_cmnd, mqe->un.mb_words[0],
				mqe->un.mb_words[1]);
		}
	}
	psli->slistat.mbox_cmd++;

	/* Post the mailbox command to the port */
	rc = lpfc_sli4_mq_put(phba->sli4_hba.mbx_wq, mqe);
	if (rc != MBX_SUCCESS) {
		lpfc_printf_log(phba, KERN_ERR, LOG_MBOX | LOG_SLI,
				"(%d):2533 Mailbox command x%x (x%x) "
				"cannot issue Data: x%x x%x\n",
				mboxq->vport ? mboxq->vport->vpi : 0,
				mboxq->u.mb.mbxCommand,
				lpfc_sli4_mbox_opcode_get(phba, mboxq),
				psli->sli_flag, MBX_NOWAIT);
		goto out_not_finished;
	}

	return rc;

out_not_finished:
	spin_lock_irqsave(&phba->hbalock, iflags);
	mboxq->u.mb.mbxStatus = MBX_NOT_FINISHED;
	__lpfc_mbox_cmpl_put(phba, mboxq);
	/* Release the token */
	psli->sli_flag &= ~LPFC_SLI_MBOX_ACTIVE;
	phba->sli.mbox_active = NULL;
	spin_unlock_irqrestore(&phba->hbalock, iflags);

	return MBX_NOT_FINISHED;
}

int
lpfc_sli_issue_mbox(struct lpfc_hba *phba, LPFC_MBOXQ_t *pmbox, uint32_t flag)
{
	return phba->lpfc_sli_issue_mbox(phba, pmbox, flag);
}

int
lpfc_mbox_api_table_setup(struct lpfc_hba *phba, uint8_t dev_grp)
{

	switch (dev_grp) {
	case LPFC_PCI_DEV_LP:
		phba->lpfc_sli_issue_mbox = lpfc_sli_issue_mbox_s3;
		phba->lpfc_sli_handle_slow_ring_event =
				lpfc_sli_handle_slow_ring_event_s3;
		phba->lpfc_sli_hbq_to_firmware = lpfc_sli_hbq_to_firmware_s3;
		phba->lpfc_sli_brdrestart = lpfc_sli_brdrestart_s3;
		phba->lpfc_sli_brdready = lpfc_sli_brdready_s3;
		break;
	case LPFC_PCI_DEV_OC:
		phba->lpfc_sli_issue_mbox = lpfc_sli_issue_mbox_s4;
		phba->lpfc_sli_handle_slow_ring_event =
				lpfc_sli_handle_slow_ring_event_s4;
		phba->lpfc_sli_hbq_to_firmware = lpfc_sli_hbq_to_firmware_s4;
		phba->lpfc_sli_brdrestart = lpfc_sli_brdrestart_s4;
		phba->lpfc_sli_brdready = lpfc_sli_brdready_s4;
		break;
	default:
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"1420 Invalid HBA PCI-device group: 0x%x\n",
				dev_grp);
		return -ENODEV;
		break;
	}
	return 0;
}

static void
__lpfc_sli_ringtx_put(struct lpfc_hba *phba, struct lpfc_sli_ring *pring,
		    struct lpfc_iocbq *piocb)
{
	/* Insert the caller's iocb in the txq tail for later processing. */
	list_add_tail(&piocb->list, &pring->txq);
	pring->txq_cnt++;
}

static struct lpfc_iocbq *
lpfc_sli_next_iocb(struct lpfc_hba *phba, struct lpfc_sli_ring *pring,
		   struct lpfc_iocbq **piocb)
{
	struct lpfc_iocbq * nextiocb;

	nextiocb = lpfc_sli_ringtx_get(phba, pring);
	if (!nextiocb) {
		nextiocb = *piocb;
		*piocb = NULL;
	}

	return nextiocb;
}

static int
__lpfc_sli_issue_iocb_s3(struct lpfc_hba *phba, uint32_t ring_number,
		    struct lpfc_iocbq *piocb, uint32_t flag)
{
	struct lpfc_iocbq *nextiocb;
	IOCB_t *iocb;
	struct lpfc_sli_ring *pring = &phba->sli.ring[ring_number];

	if (piocb->iocb_cmpl && (!piocb->vport) &&
	   (piocb->iocb.ulpCommand != CMD_ABORT_XRI_CN) &&
	   (piocb->iocb.ulpCommand != CMD_CLOSE_XRI_CN)) {
		lpfc_printf_log(phba, KERN_ERR,
				LOG_SLI | LOG_VPORT,
				"1807 IOCB x%x failed. No vport\n",
				piocb->iocb.ulpCommand);
		dump_stack();
		return IOCB_ERROR;
	}


	/* If the PCI channel is in offline state, do not post iocbs. */
	if (unlikely(pci_channel_offline(phba->pcidev)))
		return IOCB_ERROR;

	/* If HBA has a deferred error attention, fail the iocb. */
	if (unlikely(phba->hba_flag & DEFER_ERATT))
		return IOCB_ERROR;

	/*
	 * We should never get an IOCB if we are in a < LINK_DOWN state
	 */
	if (unlikely(phba->link_state < LPFC_LINK_DOWN))
		return IOCB_ERROR;

	/*
	 * Check to see if we are blocking IOCB processing because of a
	 * outstanding event.
	 */
	if (unlikely(pring->flag & LPFC_STOP_IOCB_EVENT))
		goto iocb_busy;

	if (unlikely(phba->link_state == LPFC_LINK_DOWN)) {
		/*
		 * Only CREATE_XRI, CLOSE_XRI, and QUE_RING_BUF
		 * can be issued if the link is not up.
		 */
		switch (piocb->iocb.ulpCommand) {
		case CMD_GEN_REQUEST64_CR:
		case CMD_GEN_REQUEST64_CX:
			if (!(phba->sli.sli_flag & LPFC_MENLO_MAINT) ||
				(piocb->iocb.un.genreq64.w5.hcsw.Rctl !=
					FC_RCTL_DD_UNSOL_CMD) ||
				(piocb->iocb.un.genreq64.w5.hcsw.Type !=
					MENLO_TRANSPORT_TYPE))

				goto iocb_busy;
			break;
		case CMD_QUE_RING_BUF_CN:
		case CMD_QUE_RING_BUF64_CN:
			/*
			 * For IOCBs, like QUE_RING_BUF, that have no rsp ring
			 * completion, iocb_cmpl MUST be 0.
			 */
			if (piocb->iocb_cmpl)
				piocb->iocb_cmpl = NULL;
			/*FALLTHROUGH*/
		case CMD_CREATE_XRI_CR:
		case CMD_CLOSE_XRI_CN:
		case CMD_CLOSE_XRI_CX:
			break;
		default:
			goto iocb_busy;
		}

	/*
	 * For FCP commands, we must be in a state where we can process link
	 * attention events.
	 */
	} else if (unlikely(pring->ringno == phba->sli.fcp_ring &&
			    !(phba->sli.sli_flag & LPFC_PROCESS_LA))) {
		goto iocb_busy;
	}

	while ((iocb = lpfc_sli_next_iocb_slot(phba, pring)) &&
	       (nextiocb = lpfc_sli_next_iocb(phba, pring, &piocb)))
		lpfc_sli_submit_iocb(phba, pring, iocb, nextiocb);

	if (iocb)
		lpfc_sli_update_ring(phba, pring);
	else
		lpfc_sli_update_full_ring(phba, pring);

	if (!piocb)
		return IOCB_SUCCESS;

	goto out_busy;

 iocb_busy:
	pring->stats.iocb_cmd_delay++;

 out_busy:

	if (!(flag & SLI_IOCB_RET_IOCB)) {
		__lpfc_sli_ringtx_put(phba, pring, piocb);
		return IOCB_SUCCESS;
	}

	return IOCB_BUSY;
}

static uint16_t
lpfc_sli4_bpl2sgl(struct lpfc_hba *phba, struct lpfc_iocbq *piocbq,
		struct lpfc_sglq *sglq)
{
	uint16_t xritag = NO_XRI;
	struct ulp_bde64 *bpl = NULL;
	struct ulp_bde64 bde;
	struct sli4_sge *sgl  = NULL;
	IOCB_t *icmd;
	int numBdes = 0;
	int i = 0;

	if (!piocbq || !sglq)
		return xritag;

	sgl  = (struct sli4_sge *)sglq->sgl;
	icmd = &piocbq->iocb;
	if (icmd->un.genreq64.bdl.bdeFlags == BUFF_TYPE_BLP_64) {
		numBdes = icmd->un.genreq64.bdl.bdeSize /
				sizeof(struct ulp_bde64);
		/* The addrHigh and addrLow fields within the IOCB
		 * have not been byteswapped yet so there is no
		 * need to swap them back.
		 */
		bpl  = (struct ulp_bde64 *)
			((struct lpfc_dmabuf *)piocbq->context3)->virt;

		if (!bpl)
			return xritag;

		for (i = 0; i < numBdes; i++) {
			/* Should already be byte swapped. */
			sgl->addr_hi = bpl->addrHigh;
			sgl->addr_lo = bpl->addrLow;

			if ((i+1) == numBdes)
				bf_set(lpfc_sli4_sge_last, sgl, 1);
			else
				bf_set(lpfc_sli4_sge_last, sgl, 0);
			sgl->word2 = cpu_to_le32(sgl->word2);
			/* swap the size field back to the cpu so we
			 * can assign it to the sgl.
			 */
			bde.tus.w = le32_to_cpu(bpl->tus.w);
			sgl->sge_len = cpu_to_le32(bde.tus.f.bdeSize);
			bpl++;
			sgl++;
		}
	} else if (icmd->un.genreq64.bdl.bdeFlags == BUFF_TYPE_BDE_64) {
			/* The addrHigh and addrLow fields of the BDE have not
			 * been byteswapped yet so they need to be swapped
			 * before putting them in the sgl.
			 */
			sgl->addr_hi =
				cpu_to_le32(icmd->un.genreq64.bdl.addrHigh);
			sgl->addr_lo =
				cpu_to_le32(icmd->un.genreq64.bdl.addrLow);
			bf_set(lpfc_sli4_sge_last, sgl, 1);
			sgl->word2 = cpu_to_le32(sgl->word2);
			sgl->sge_len =
				cpu_to_le32(icmd->un.genreq64.bdl.bdeSize);
	}
	return sglq->sli4_xritag;
}

static uint32_t
lpfc_sli4_scmd_to_wqidx_distr(struct lpfc_hba *phba)
{
	++phba->fcp_qidx;
	if (phba->fcp_qidx >= phba->cfg_fcp_wq_count)
		phba->fcp_qidx = 0;

	return phba->fcp_qidx;
}

static int
lpfc_sli4_iocb2wqe(struct lpfc_hba *phba, struct lpfc_iocbq *iocbq,
		union lpfc_wqe *wqe)
{
	uint32_t xmit_len = 0, total_len = 0;
	uint8_t ct = 0;
	uint32_t fip;
	uint32_t abort_tag;
	uint8_t command_type = ELS_COMMAND_NON_FIP;
	uint8_t cmnd;
	uint16_t xritag;
	struct ulp_bde64 *bpl = NULL;
	uint32_t els_id = ELS_ID_DEFAULT;
	int numBdes, i;
	struct ulp_bde64 bde;

	fip = phba->hba_flag & HBA_FIP_SUPPORT;
	/* The fcp commands will set command type */
	if (iocbq->iocb_flag &  LPFC_IO_FCP)
		command_type = FCP_COMMAND;
	else if (fip && (iocbq->iocb_flag & LPFC_FIP_ELS_ID_MASK))
		command_type = ELS_COMMAND_FIP;
	else
		command_type = ELS_COMMAND_NON_FIP;

	/* Some of the fields are in the right position already */
	memcpy(wqe, &iocbq->iocb, sizeof(union lpfc_wqe));
	abort_tag = (uint32_t) iocbq->iotag;
	xritag = iocbq->sli4_xritag;
	wqe->words[7] = 0; /* The ct field has moved so reset */
	/* words0-2 bpl convert bde */
	if (iocbq->iocb.un.genreq64.bdl.bdeFlags == BUFF_TYPE_BLP_64) {
		numBdes = iocbq->iocb.un.genreq64.bdl.bdeSize /
				sizeof(struct ulp_bde64);
		bpl  = (struct ulp_bde64 *)
			((struct lpfc_dmabuf *)iocbq->context3)->virt;
		if (!bpl)
			return IOCB_ERROR;

		/* Should already be byte swapped. */
		wqe->generic.bde.addrHigh =  le32_to_cpu(bpl->addrHigh);
		wqe->generic.bde.addrLow =  le32_to_cpu(bpl->addrLow);
		/* swap the size field back to the cpu so we
		 * can assign it to the sgl.
		 */
		wqe->generic.bde.tus.w  = le32_to_cpu(bpl->tus.w);
		xmit_len = wqe->generic.bde.tus.f.bdeSize;
		total_len = 0;
		for (i = 0; i < numBdes; i++) {
			bde.tus.w  = le32_to_cpu(bpl[i].tus.w);
			total_len += bde.tus.f.bdeSize;
		}
	} else
		xmit_len = iocbq->iocb.un.fcpi64.bdl.bdeSize;

	iocbq->iocb.ulpIoTag = iocbq->iotag;
	cmnd = iocbq->iocb.ulpCommand;

	switch (iocbq->iocb.ulpCommand) {
	case CMD_ELS_REQUEST64_CR:
		if (!iocbq->iocb.ulpLe) {
			lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"2007 Only Limited Edition cmd Format"
				" supported 0x%x\n",
				iocbq->iocb.ulpCommand);
			return IOCB_ERROR;
		}
		wqe->els_req.payload_len = xmit_len;
		/* Els_reguest64 has a TMO */
		bf_set(wqe_tmo, &wqe->els_req.wqe_com,
			iocbq->iocb.ulpTimeout);
		/* Need a VF for word 4 set the vf bit*/
		bf_set(els_req64_vf, &wqe->els_req, 0);
		/* And a VFID for word 12 */
		bf_set(els_req64_vfid, &wqe->els_req, 0);
		/*
		 * Set ct field to 3, indicates that the context_tag field
		 * contains the FCFI and remote N_Port_ID is
		 * in word 5.
		 */

		ct = ((iocbq->iocb.ulpCt_h << 1) | iocbq->iocb.ulpCt_l);
		bf_set(lpfc_wqe_gen_context, &wqe->generic,
				iocbq->iocb.ulpContext);

		bf_set(lpfc_wqe_gen_ct, &wqe->generic, ct);
		bf_set(lpfc_wqe_gen_pu, &wqe->generic, 0);
		/* CCP CCPE PV PRI in word10 were set in the memcpy */

		if (command_type == ELS_COMMAND_FIP) {
			els_id = ((iocbq->iocb_flag & LPFC_FIP_ELS_ID_MASK)
					>> LPFC_FIP_ELS_ID_SHIFT);
		}
		bf_set(lpfc_wqe_gen_els_id, &wqe->generic, els_id);

	break;
	case CMD_XMIT_SEQUENCE64_CX:
		bf_set(lpfc_wqe_gen_context, &wqe->generic,
					iocbq->iocb.un.ulpWord[3]);
		wqe->generic.word3 = 0;
		bf_set(wqe_rcvoxid, &wqe->generic, iocbq->iocb.ulpContext);
		/* The entire sequence is transmitted for this IOCB */
		xmit_len = total_len;
		cmnd = CMD_XMIT_SEQUENCE64_CR;
	case CMD_XMIT_SEQUENCE64_CR:
		/* word3 iocb=io_tag32 wqe=payload_offset */
		/* payload offset used for multilpe outstanding
		 * sequences on the same exchange
		 */
		wqe->words[3] = 0;
		/* word4 relative_offset memcpy */
		/* word5 r_ctl/df_ctl memcpy */
		bf_set(lpfc_wqe_gen_pu, &wqe->generic, 0);
		wqe->xmit_sequence.xmit_len = xmit_len;
		command_type = OTHER_COMMAND;
	break;
	case CMD_XMIT_BCAST64_CN:
		/* word3 iocb=iotag32 wqe=payload_len */
		wqe->words[3] = 0; /* no definition for this in wqe */
		/* word4 iocb=rsvd wqe=rsvd */
		/* word5 iocb=rctl/type/df_ctl wqe=rctl/type/df_ctl memcpy */
		/* word6 iocb=ctxt_tag/io_tag wqe=ctxt_tag/xri */
		bf_set(lpfc_wqe_gen_ct, &wqe->generic,
			((iocbq->iocb.ulpCt_h << 1) | iocbq->iocb.ulpCt_l));
	break;
	case CMD_FCP_IWRITE64_CR:
		command_type = FCP_COMMAND_DATA_OUT;
		/* The struct for wqe fcp_iwrite has 3 fields that are somewhat
		 * confusing.
		 * word3 is payload_len: byte offset to the sgl entry for the
		 * fcp_command.
		 * word4 is total xfer len, same as the IOCB->ulpParameter.
		 * word5 is initial xfer len 0 = wait for xfer-ready
		 */

		/* Always wait for xfer-ready before sending data */
		wqe->fcp_iwrite.initial_xfer_len = 0;
		/* word 4 (xfer length) should have been set on the memcpy */

	/* allow write to fall through to read */
	case CMD_FCP_IREAD64_CR:
		/* FCP_CMD is always the 1st sgl entry */
		wqe->fcp_iread.payload_len =
			xmit_len + sizeof(struct fcp_rsp);

		/* word 4 (xfer length) should have been set on the memcpy */

		bf_set(lpfc_wqe_gen_erp, &wqe->generic,
			iocbq->iocb.ulpFCP2Rcvy);
		bf_set(lpfc_wqe_gen_lnk, &wqe->generic, iocbq->iocb.ulpXS);
		/* The XC bit and the XS bit are similar. The driver never
		 * tracked whether or not the exchange was previouslly open.
		 * XC = Exchange create, 0 is create. 1 is already open.
		 * XS = link cmd: 1 do not close the exchange after command.
		 * XS = 0 close exchange when command completes.
		 * The only time we would not set the XC bit is when the XS bit
		 * is set and we are sending our 2nd or greater command on
		 * this exchange.
		 */
		/* Always open the exchange */
		bf_set(wqe_xc, &wqe->fcp_iread.wqe_com, 0);

		wqe->words[10] &= 0xffff0000; /* zero out ebde count */
		bf_set(lpfc_wqe_gen_pu, &wqe->generic, iocbq->iocb.ulpPU);
		break;
	case CMD_FCP_ICMND64_CR:
		/* Always open the exchange */
		bf_set(wqe_xc, &wqe->fcp_iread.wqe_com, 0);

		wqe->words[4] = 0;
		wqe->words[10] &= 0xffff0000; /* zero out ebde count */
		bf_set(lpfc_wqe_gen_pu, &wqe->generic, 0);
	break;
	case CMD_GEN_REQUEST64_CR:
		/* word3 command length is described as byte offset to the
		 * rsp_data. Would always be 16, sizeof(struct sli4_sge)
		 * sgl[0] = cmnd
		 * sgl[1] = rsp.
		 *
		 */
		wqe->gen_req.command_len = xmit_len;
		/* Word4 parameter  copied in the memcpy */
		/* Word5 [rctl, type, df_ctl, la] copied in memcpy */
		/* word6 context tag copied in memcpy */
		if (iocbq->iocb.ulpCt_h  || iocbq->iocb.ulpCt_l) {
			ct = ((iocbq->iocb.ulpCt_h << 1) | iocbq->iocb.ulpCt_l);
			lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"2015 Invalid CT %x command 0x%x\n",
				ct, iocbq->iocb.ulpCommand);
			return IOCB_ERROR;
		}
		bf_set(lpfc_wqe_gen_ct, &wqe->generic, 0);
		bf_set(wqe_tmo, &wqe->gen_req.wqe_com,
			iocbq->iocb.ulpTimeout);

		bf_set(lpfc_wqe_gen_pu, &wqe->generic, iocbq->iocb.ulpPU);
		command_type = OTHER_COMMAND;
	break;
	case CMD_XMIT_ELS_RSP64_CX:
		/* words0-2 BDE memcpy */
		/* word3 iocb=iotag32 wqe=rsvd */
		wqe->words[3] = 0;
		/* word4 iocb=did wge=rsvd. */
		wqe->words[4] = 0;
		/* word5 iocb=rsvd wge=did */
		bf_set(wqe_els_did, &wqe->xmit_els_rsp.wqe_dest,
			 iocbq->iocb.un.elsreq64.remoteID);

		bf_set(lpfc_wqe_gen_ct, &wqe->generic,
			((iocbq->iocb.ulpCt_h << 1) | iocbq->iocb.ulpCt_l));

		bf_set(lpfc_wqe_gen_pu, &wqe->generic, iocbq->iocb.ulpPU);
		bf_set(wqe_rcvoxid, &wqe->generic, iocbq->iocb.ulpContext);
		if (!iocbq->iocb.ulpCt_h && iocbq->iocb.ulpCt_l)
			bf_set(lpfc_wqe_gen_context, &wqe->generic,
			       iocbq->vport->vpi + phba->vpi_base);
		command_type = OTHER_COMMAND;
	break;
	case CMD_CLOSE_XRI_CN:
	case CMD_ABORT_XRI_CN:
	case CMD_ABORT_XRI_CX:
		/* words 0-2 memcpy should be 0 rserved */
		/* port will send abts */
		if (iocbq->iocb.ulpCommand == CMD_CLOSE_XRI_CN)
			/*
			 * The link is down so the fw does not need to send abts
			 * on the wire.
			 */
			bf_set(abort_cmd_ia, &wqe->abort_cmd, 1);
		else
			bf_set(abort_cmd_ia, &wqe->abort_cmd, 0);
		bf_set(abort_cmd_criteria, &wqe->abort_cmd, T_XRI_TAG);
		wqe->words[5] = 0;
		bf_set(lpfc_wqe_gen_ct, &wqe->generic,
			((iocbq->iocb.ulpCt_h << 1) | iocbq->iocb.ulpCt_l));
		abort_tag = iocbq->iocb.un.acxri.abortIoTag;
		/*
		 * The abort handler will send us CMD_ABORT_XRI_CN or
		 * CMD_CLOSE_XRI_CN and the fw only accepts CMD_ABORT_XRI_CX
		 */
		bf_set(lpfc_wqe_gen_command, &wqe->generic, CMD_ABORT_XRI_CX);
		cmnd = CMD_ABORT_XRI_CX;
		command_type = OTHER_COMMAND;
		xritag = 0;
	break;
	case CMD_XMIT_BLS_RSP64_CX:
		/* As BLS ABTS-ACC WQE is very different from other WQEs,
		 * we re-construct this WQE here based on information in
		 * iocbq from scratch.
		 */
		memset(wqe, 0, sizeof(union lpfc_wqe));
		/* OX_ID is invariable to who sent ABTS to CT exchange */
		bf_set(xmit_bls_rsp64_oxid, &wqe->xmit_bls_rsp,
		       bf_get(lpfc_abts_oxid, &iocbq->iocb.un.bls_acc));
		if (bf_get(lpfc_abts_orig, &iocbq->iocb.un.bls_acc) ==
		    LPFC_ABTS_UNSOL_INT) {
			/* ABTS sent by initiator to CT exchange, the
			 * RX_ID field will be filled with the newly
			 * allocated responder XRI.
			 */
			bf_set(xmit_bls_rsp64_rxid, &wqe->xmit_bls_rsp,
			       iocbq->sli4_xritag);
		} else {
			/* ABTS sent by responder to CT exchange, the
			 * RX_ID field will be filled with the responder
			 * RX_ID from ABTS.
			 */
			bf_set(xmit_bls_rsp64_rxid, &wqe->xmit_bls_rsp,
			       bf_get(lpfc_abts_rxid, &iocbq->iocb.un.bls_acc));
		}
		bf_set(xmit_bls_rsp64_seqcnthi, &wqe->xmit_bls_rsp, 0xffff);
		bf_set(wqe_xmit_bls_pt, &wqe->xmit_bls_rsp.wqe_dest, 0x1);
		bf_set(wqe_ctxt_tag, &wqe->xmit_bls_rsp.wqe_com,
		       iocbq->iocb.ulpContext);
		/* Overwrite the pre-set comnd type with OTHER_COMMAND */
		command_type = OTHER_COMMAND;
	break;
	case CMD_XRI_ABORTED_CX:
	case CMD_CREATE_XRI_CR: /* Do we expect to use this? */
		/* words0-2 are all 0's no bde */
		/* word3 and word4 are rsvrd */
		wqe->words[3] = 0;
		wqe->words[4] = 0;
		/* word5 iocb=rsvd wge=did */
		/* There is no remote port id in the IOCB? */
		/* Let this fall through and fail */
	case CMD_IOCB_FCP_IBIDIR64_CR: /* bidirectional xfer */
	case CMD_FCP_TSEND64_CX: /* Target mode send xfer-ready */
	case CMD_FCP_TRSP64_CX: /* Target mode rcv */
	case CMD_FCP_AUTO_TRSP_CX: /* Auto target rsp */
	default:
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"2014 Invalid command 0x%x\n",
				iocbq->iocb.ulpCommand);
		return IOCB_ERROR;
	break;

	}
	bf_set(lpfc_wqe_gen_xri, &wqe->generic, xritag);
	bf_set(lpfc_wqe_gen_request_tag, &wqe->generic, iocbq->iotag);
	wqe->generic.abort_tag = abort_tag;
	bf_set(lpfc_wqe_gen_cmd_type, &wqe->generic, command_type);
	bf_set(lpfc_wqe_gen_command, &wqe->generic, cmnd);
	bf_set(lpfc_wqe_gen_class, &wqe->generic, iocbq->iocb.ulpClass);
	bf_set(lpfc_wqe_gen_cq_id, &wqe->generic, LPFC_WQE_CQ_ID_DEFAULT);

	return 0;
}

static int
__lpfc_sli_issue_iocb_s4(struct lpfc_hba *phba, uint32_t ring_number,
			 struct lpfc_iocbq *piocb, uint32_t flag)
{
	struct lpfc_sglq *sglq;
	uint16_t xritag;
	union lpfc_wqe wqe;
	struct lpfc_sli_ring *pring = &phba->sli.ring[ring_number];

	if (piocb->sli4_xritag == NO_XRI) {
		if (piocb->iocb.ulpCommand == CMD_ABORT_XRI_CN ||
		    piocb->iocb.ulpCommand == CMD_CLOSE_XRI_CN)
			sglq = NULL;
		else {
			sglq = __lpfc_sli_get_sglq(phba);
			if (!sglq)
				return IOCB_ERROR;
			piocb->sli4_xritag = sglq->sli4_xritag;
		}
	} else if (piocb->iocb_flag &  LPFC_IO_FCP) {
		sglq = NULL; /* These IO's already have an XRI and
			      * a mapped sgl.
			      */
	} else {
		/* This is a continuation of a commandi,(CX) so this
		 * sglq is on the active list
		 */
		sglq = __lpfc_get_active_sglq(phba, piocb->sli4_xritag);
		if (!sglq)
			return IOCB_ERROR;
	}

	if (sglq) {
		xritag = lpfc_sli4_bpl2sgl(phba, piocb, sglq);
		if (xritag != sglq->sli4_xritag)
			return IOCB_ERROR;
	}

	if (lpfc_sli4_iocb2wqe(phba, piocb, &wqe))
		return IOCB_ERROR;

	if ((piocb->iocb_flag & LPFC_IO_FCP) ||
		(piocb->iocb_flag & LPFC_USE_FCPWQIDX)) {
		/*
		 * For FCP command IOCB, get a new WQ index to distribute
		 * WQE across the WQsr. On the other hand, for abort IOCB,
		 * it carries the same WQ index to the original command
		 * IOCB.
		 */
		if (piocb->iocb_flag & LPFC_IO_FCP)
			piocb->fcp_wqidx = lpfc_sli4_scmd_to_wqidx_distr(phba);
		if (lpfc_sli4_wq_put(phba->sli4_hba.fcp_wq[piocb->fcp_wqidx],
				     &wqe))
			return IOCB_ERROR;
	} else {
		if (lpfc_sli4_wq_put(phba->sli4_hba.els_wq, &wqe))
			return IOCB_ERROR;
	}
	lpfc_sli_ringtxcmpl_put(phba, pring, piocb);

	return 0;
}

static inline int
__lpfc_sli_issue_iocb(struct lpfc_hba *phba, uint32_t ring_number,
		struct lpfc_iocbq *piocb, uint32_t flag)
{
	return phba->__lpfc_sli_issue_iocb(phba, ring_number, piocb, flag);
}

int
lpfc_sli_api_table_setup(struct lpfc_hba *phba, uint8_t dev_grp)
{

	switch (dev_grp) {
	case LPFC_PCI_DEV_LP:
		phba->__lpfc_sli_issue_iocb = __lpfc_sli_issue_iocb_s3;
		phba->__lpfc_sli_release_iocbq = __lpfc_sli_release_iocbq_s3;
		break;
	case LPFC_PCI_DEV_OC:
		phba->__lpfc_sli_issue_iocb = __lpfc_sli_issue_iocb_s4;
		phba->__lpfc_sli_release_iocbq = __lpfc_sli_release_iocbq_s4;
		break;
	default:
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"1419 Invalid HBA PCI-device group: 0x%x\n",
				dev_grp);
		return -ENODEV;
		break;
	}
	phba->lpfc_get_iocb_from_iocbq = lpfc_get_iocb_from_iocbq;
	return 0;
}

int
lpfc_sli_issue_iocb(struct lpfc_hba *phba, uint32_t ring_number,
		    struct lpfc_iocbq *piocb, uint32_t flag)
{
	unsigned long iflags;
	int rc;

	spin_lock_irqsave(&phba->hbalock, iflags);
	rc = __lpfc_sli_issue_iocb(phba, ring_number, piocb, flag);
	spin_unlock_irqrestore(&phba->hbalock, iflags);

	return rc;
}

static int
lpfc_extra_ring_setup( struct lpfc_hba *phba)
{
	struct lpfc_sli *psli;
	struct lpfc_sli_ring *pring;

	psli = &phba->sli;

	/* Adjust cmd/rsp ring iocb entries more evenly */

	/* Take some away from the FCP ring */
	pring = &psli->ring[psli->fcp_ring];
	pring->numCiocb -= SLI2_IOCB_CMD_R1XTRA_ENTRIES;
	pring->numRiocb -= SLI2_IOCB_RSP_R1XTRA_ENTRIES;
	pring->numCiocb -= SLI2_IOCB_CMD_R3XTRA_ENTRIES;
	pring->numRiocb -= SLI2_IOCB_RSP_R3XTRA_ENTRIES;

	/* and give them to the extra ring */
	pring = &psli->ring[psli->extra_ring];

	pring->numCiocb += SLI2_IOCB_CMD_R1XTRA_ENTRIES;
	pring->numRiocb += SLI2_IOCB_RSP_R1XTRA_ENTRIES;
	pring->numCiocb += SLI2_IOCB_CMD_R3XTRA_ENTRIES;
	pring->numRiocb += SLI2_IOCB_RSP_R3XTRA_ENTRIES;

	/* Setup default profile for this ring */
	pring->iotag_max = 4096;
	pring->num_mask = 1;
	pring->prt[0].profile = 0;      /* Mask 0 */
	pring->prt[0].rctl = phba->cfg_multi_ring_rctl;
	pring->prt[0].type = phba->cfg_multi_ring_type;
	pring->prt[0].lpfc_sli_rcv_unsol_event = NULL;
	return 0;
}

static void
lpfc_sli_async_event_handler(struct lpfc_hba * phba,
	struct lpfc_sli_ring * pring, struct lpfc_iocbq * iocbq)
{
	IOCB_t *icmd;
	uint16_t evt_code;
	uint16_t temp;
	struct temp_event temp_event_data;
	struct Scsi_Host *shost;
	uint32_t *iocb_w;

	icmd = &iocbq->iocb;
	evt_code = icmd->un.asyncstat.evt_code;
	temp = icmd->ulpContext;

	if ((evt_code != ASYNC_TEMP_WARN) &&
		(evt_code != ASYNC_TEMP_SAFE)) {
		iocb_w = (uint32_t *) icmd;
		lpfc_printf_log(phba,
			KERN_ERR,
			LOG_SLI,
			"0346 Ring %d handler: unexpected ASYNC_STATUS"
			" evt_code 0x%x\n"
			"W0  0x%08x W1  0x%08x W2  0x%08x W3  0x%08x\n"
			"W4  0x%08x W5  0x%08x W6  0x%08x W7  0x%08x\n"
			"W8  0x%08x W9  0x%08x W10 0x%08x W11 0x%08x\n"
			"W12 0x%08x W13 0x%08x W14 0x%08x W15 0x%08x\n",
			pring->ringno,
			icmd->un.asyncstat.evt_code,
			iocb_w[0], iocb_w[1], iocb_w[2], iocb_w[3],
			iocb_w[4], iocb_w[5], iocb_w[6], iocb_w[7],
			iocb_w[8], iocb_w[9], iocb_w[10], iocb_w[11],
			iocb_w[12], iocb_w[13], iocb_w[14], iocb_w[15]);

		return;
	}
	temp_event_data.data = (uint32_t)temp;
	temp_event_data.event_type = FC_REG_TEMPERATURE_EVENT;
	if (evt_code == ASYNC_TEMP_WARN) {
		temp_event_data.event_code = LPFC_THRESHOLD_TEMP;
		lpfc_printf_log(phba,
				KERN_ERR,
				LOG_TEMP,
				"0347 Adapter is very hot, please take "
				"corrective action. temperature : %d Celsius\n",
				temp);
	}
	if (evt_code == ASYNC_TEMP_SAFE) {
		temp_event_data.event_code = LPFC_NORMAL_TEMP;
		lpfc_printf_log(phba,
				KERN_ERR,
				LOG_TEMP,
				"0340 Adapter temperature is OK now. "
				"temperature : %d Celsius\n",
				temp);
	}

	/* Send temperature change event to applications */
	shost = lpfc_shost_from_vport(phba->pport);
	fc_host_post_vendor_event(shost, fc_get_event_number(),
		sizeof(temp_event_data), (char *) &temp_event_data,
		LPFC_NL_VENDOR_ID);

}


int
lpfc_sli_setup(struct lpfc_hba *phba)
{
	int i, totiocbsize = 0;
	struct lpfc_sli *psli = &phba->sli;
	struct lpfc_sli_ring *pring;

	psli->num_rings = MAX_CONFIGURED_RINGS;
	psli->sli_flag = 0;
	psli->fcp_ring = LPFC_FCP_RING;
	psli->next_ring = LPFC_FCP_NEXT_RING;
	psli->extra_ring = LPFC_EXTRA_RING;

	psli->iocbq_lookup = NULL;
	psli->iocbq_lookup_len = 0;
	psli->last_iotag = 0;

	for (i = 0; i < psli->num_rings; i++) {
		pring = &psli->ring[i];
		switch (i) {
		case LPFC_FCP_RING:	/* ring 0 - FCP */
			/* numCiocb and numRiocb are used in config_port */
			pring->numCiocb = SLI2_IOCB_CMD_R0_ENTRIES;
			pring->numRiocb = SLI2_IOCB_RSP_R0_ENTRIES;
			pring->numCiocb += SLI2_IOCB_CMD_R1XTRA_ENTRIES;
			pring->numRiocb += SLI2_IOCB_RSP_R1XTRA_ENTRIES;
			pring->numCiocb += SLI2_IOCB_CMD_R3XTRA_ENTRIES;
			pring->numRiocb += SLI2_IOCB_RSP_R3XTRA_ENTRIES;
			pring->sizeCiocb = (phba->sli_rev == 3) ?
							SLI3_IOCB_CMD_SIZE :
							SLI2_IOCB_CMD_SIZE;
			pring->sizeRiocb = (phba->sli_rev == 3) ?
							SLI3_IOCB_RSP_SIZE :
							SLI2_IOCB_RSP_SIZE;
			pring->iotag_ctr = 0;
			pring->iotag_max =
			    (phba->cfg_hba_queue_depth * 2);
			pring->fast_iotag = pring->iotag_max;
			pring->num_mask = 0;
			break;
		case LPFC_EXTRA_RING:	/* ring 1 - EXTRA */
			/* numCiocb and numRiocb are used in config_port */
			pring->numCiocb = SLI2_IOCB_CMD_R1_ENTRIES;
			pring->numRiocb = SLI2_IOCB_RSP_R1_ENTRIES;
			pring->sizeCiocb = (phba->sli_rev == 3) ?
							SLI3_IOCB_CMD_SIZE :
							SLI2_IOCB_CMD_SIZE;
			pring->sizeRiocb = (phba->sli_rev == 3) ?
							SLI3_IOCB_RSP_SIZE :
							SLI2_IOCB_RSP_SIZE;
			pring->iotag_max = phba->cfg_hba_queue_depth;
			pring->num_mask = 0;
			break;
		case LPFC_ELS_RING:	/* ring 2 - ELS / CT */
			/* numCiocb and numRiocb are used in config_port */
			pring->numCiocb = SLI2_IOCB_CMD_R2_ENTRIES;
			pring->numRiocb = SLI2_IOCB_RSP_R2_ENTRIES;
			pring->sizeCiocb = (phba->sli_rev == 3) ?
							SLI3_IOCB_CMD_SIZE :
							SLI2_IOCB_CMD_SIZE;
			pring->sizeRiocb = (phba->sli_rev == 3) ?
							SLI3_IOCB_RSP_SIZE :
							SLI2_IOCB_RSP_SIZE;
			pring->fast_iotag = 0;
			pring->iotag_ctr = 0;
			pring->iotag_max = 4096;
			pring->lpfc_sli_rcv_async_status =
				lpfc_sli_async_event_handler;
			pring->num_mask = LPFC_MAX_RING_MASK;
			pring->prt[0].profile = 0;	/* Mask 0 */
			pring->prt[0].rctl = FC_RCTL_ELS_REQ;
			pring->prt[0].type = FC_TYPE_ELS;
			pring->prt[0].lpfc_sli_rcv_unsol_event =
			    lpfc_els_unsol_event;
			pring->prt[1].profile = 0;	/* Mask 1 */
			pring->prt[1].rctl = FC_RCTL_ELS_REP;
			pring->prt[1].type = FC_TYPE_ELS;
			pring->prt[1].lpfc_sli_rcv_unsol_event =
			    lpfc_els_unsol_event;
			pring->prt[2].profile = 0;	/* Mask 2 */
			/* NameServer Inquiry */
			pring->prt[2].rctl = FC_RCTL_DD_UNSOL_CTL;
			/* NameServer */
			pring->prt[2].type = FC_TYPE_CT;
			pring->prt[2].lpfc_sli_rcv_unsol_event =
			    lpfc_ct_unsol_event;
			pring->prt[3].profile = 0;	/* Mask 3 */
			/* NameServer response */
			pring->prt[3].rctl = FC_RCTL_DD_SOL_CTL;
			/* NameServer */
			pring->prt[3].type = FC_TYPE_CT;
			pring->prt[3].lpfc_sli_rcv_unsol_event =
			    lpfc_ct_unsol_event;
			/* abort unsolicited sequence */
			pring->prt[4].profile = 0;	/* Mask 4 */
			pring->prt[4].rctl = FC_RCTL_BA_ABTS;
			pring->prt[4].type = FC_TYPE_BLS;
			pring->prt[4].lpfc_sli_rcv_unsol_event =
			    lpfc_sli4_ct_abort_unsol_event;
			break;
		}
		totiocbsize += (pring->numCiocb * pring->sizeCiocb) +
				(pring->numRiocb * pring->sizeRiocb);
	}
	if (totiocbsize > MAX_SLIM_IOCB_SIZE) {
		/* Too many cmd / rsp ring entries in SLI2 SLIM */
		printk(KERN_ERR "%d:0462 Too many cmd / rsp ring entries in "
		       "SLI2 SLIM Data: x%x x%lx\n",
		       phba->brd_no, totiocbsize,
		       (unsigned long) MAX_SLIM_IOCB_SIZE);
	}
	if (phba->cfg_multi_ring_support == 2)
		lpfc_extra_ring_setup(phba);

	return 0;
}

int
lpfc_sli_queue_setup(struct lpfc_hba *phba)
{
	struct lpfc_sli *psli;
	struct lpfc_sli_ring *pring;
	int i;

	psli = &phba->sli;
	spin_lock_irq(&phba->hbalock);
	INIT_LIST_HEAD(&psli->mboxq);
	INIT_LIST_HEAD(&psli->mboxq_cmpl);
	/* Initialize list headers for txq and txcmplq as double linked lists */
	for (i = 0; i < psli->num_rings; i++) {
		pring = &psli->ring[i];
		pring->ringno = i;
		pring->next_cmdidx  = 0;
		pring->local_getidx = 0;
		pring->cmdidx = 0;
		INIT_LIST_HEAD(&pring->txq);
		INIT_LIST_HEAD(&pring->txcmplq);
		INIT_LIST_HEAD(&pring->iocb_continueq);
		INIT_LIST_HEAD(&pring->iocb_continue_saveq);
		INIT_LIST_HEAD(&pring->postbufq);
	}
	spin_unlock_irq(&phba->hbalock);
	return 1;
}

static void
lpfc_sli_mbox_sys_flush(struct lpfc_hba *phba)
{
	LIST_HEAD(completions);
	struct lpfc_sli *psli = &phba->sli;
	LPFC_MBOXQ_t *pmb;
	unsigned long iflag;

	/* Flush all the mailbox commands in the mbox system */
	spin_lock_irqsave(&phba->hbalock, iflag);
	/* The pending mailbox command queue */
	list_splice_init(&phba->sli.mboxq, &completions);
	/* The outstanding active mailbox command */
	if (psli->mbox_active) {
		list_add_tail(&psli->mbox_active->list, &completions);
		psli->mbox_active = NULL;
		psli->sli_flag &= ~LPFC_SLI_MBOX_ACTIVE;
	}
	/* The completed mailbox command queue */
	list_splice_init(&phba->sli.mboxq_cmpl, &completions);
	spin_unlock_irqrestore(&phba->hbalock, iflag);

	/* Return all flushed mailbox commands with MBX_NOT_FINISHED status */
	while (!list_empty(&completions)) {
		list_remove_head(&completions, pmb, LPFC_MBOXQ_t, list);
		pmb->u.mb.mbxStatus = MBX_NOT_FINISHED;
		if (pmb->mbox_cmpl)
			pmb->mbox_cmpl(phba, pmb);
	}
}

int
lpfc_sli_host_down(struct lpfc_vport *vport)
{
	LIST_HEAD(completions);
	struct lpfc_hba *phba = vport->phba;
	struct lpfc_sli *psli = &phba->sli;
	struct lpfc_sli_ring *pring;
	struct lpfc_iocbq *iocb, *next_iocb;
	int i;
	unsigned long flags = 0;
	uint16_t prev_pring_flag;

	lpfc_cleanup_discovery_resources(vport);

	spin_lock_irqsave(&phba->hbalock, flags);
	for (i = 0; i < psli->num_rings; i++) {
		pring = &psli->ring[i];
		prev_pring_flag = pring->flag;
		/* Only slow rings */
		if (pring->ringno == LPFC_ELS_RING) {
			pring->flag |= LPFC_DEFERRED_RING_EVENT;
			/* Set the lpfc data pending flag */
			set_bit(LPFC_DATA_READY, &phba->data_flags);
		}
		/*
		 * Error everything on the txq since these iocbs have not been
		 * given to the FW yet.
		 */
		list_for_each_entry_safe(iocb, next_iocb, &pring->txq, list) {
			if (iocb->vport != vport)
				continue;
			list_move_tail(&iocb->list, &completions);
			pring->txq_cnt--;
		}

		/* Next issue ABTS for everything on the txcmplq */
		list_for_each_entry_safe(iocb, next_iocb, &pring->txcmplq,
									list) {
			if (iocb->vport != vport)
				continue;
			lpfc_sli_issue_abort_iotag(phba, pring, iocb);
		}

		pring->flag = prev_pring_flag;
	}

	spin_unlock_irqrestore(&phba->hbalock, flags);

	/* Cancel all the IOCBs from the completions list */
	lpfc_sli_cancel_iocbs(phba, &completions, IOSTAT_LOCAL_REJECT,
			      IOERR_SLI_DOWN);
	return 1;
}

int
lpfc_sli_hba_down(struct lpfc_hba *phba)
{
	LIST_HEAD(completions);
	struct lpfc_sli *psli = &phba->sli;
	struct lpfc_sli_ring *pring;
	struct lpfc_dmabuf *buf_ptr;
	unsigned long flags = 0;
	int i;

	/* Shutdown the mailbox command sub-system */
	lpfc_sli_mbox_sys_shutdown(phba);

	lpfc_hba_down_prep(phba);

	lpfc_fabric_abort_hba(phba);

	spin_lock_irqsave(&phba->hbalock, flags);
	for (i = 0; i < psli->num_rings; i++) {
		pring = &psli->ring[i];
		/* Only slow rings */
		if (pring->ringno == LPFC_ELS_RING) {
			pring->flag |= LPFC_DEFERRED_RING_EVENT;
			/* Set the lpfc data pending flag */
			set_bit(LPFC_DATA_READY, &phba->data_flags);
		}

		/*
		 * Error everything on the txq since these iocbs have not been
		 * given to the FW yet.
		 */
		list_splice_init(&pring->txq, &completions);
		pring->txq_cnt = 0;

	}
	spin_unlock_irqrestore(&phba->hbalock, flags);

	/* Cancel all the IOCBs from the completions list */
	lpfc_sli_cancel_iocbs(phba, &completions, IOSTAT_LOCAL_REJECT,
			      IOERR_SLI_DOWN);

	spin_lock_irqsave(&phba->hbalock, flags);
	list_splice_init(&phba->elsbuf, &completions);
	phba->elsbuf_cnt = 0;
	phba->elsbuf_prev_cnt = 0;
	spin_unlock_irqrestore(&phba->hbalock, flags);

	while (!list_empty(&completions)) {
		list_remove_head(&completions, buf_ptr,
			struct lpfc_dmabuf, list);
		lpfc_mbuf_free(phba, buf_ptr->virt, buf_ptr->phys);
		kfree(buf_ptr);
	}

	/* Return any active mbox cmds */
	del_timer_sync(&psli->mbox_tmo);

	spin_lock_irqsave(&phba->pport->work_port_lock, flags);
	phba->pport->work_port_events &= ~WORKER_MBOX_TMO;
	spin_unlock_irqrestore(&phba->pport->work_port_lock, flags);

	return 1;
}

int
lpfc_sli4_hba_down(struct lpfc_hba *phba)
{
	/* Stop the SLI4 device port */
	lpfc_stop_port(phba);

	/* Tear down the queues in the HBA */
	lpfc_sli4_queue_unset(phba);

	/* unregister default FCFI from the HBA */
	lpfc_sli4_fcfi_unreg(phba, phba->fcf.fcfi);

	return 1;
}

void
lpfc_sli_pcimem_bcopy(void *srcp, void *destp, uint32_t cnt)
{
	uint32_t *src = srcp;
	uint32_t *dest = destp;
	uint32_t ldata;
	int i;

	for (i = 0; i < (int)cnt; i += sizeof (uint32_t)) {
		ldata = *src;
		ldata = le32_to_cpu(ldata);
		*dest = ldata;
		src++;
		dest++;
	}
}


void
lpfc_sli_bemem_bcopy(void *srcp, void *destp, uint32_t cnt)
{
	uint32_t *src = srcp;
	uint32_t *dest = destp;
	uint32_t ldata;
	int i;

	for (i = 0; i < (int)cnt; i += sizeof(uint32_t)) {
		ldata = *src;
		ldata = be32_to_cpu(ldata);
		*dest = ldata;
		src++;
		dest++;
	}
}

int
lpfc_sli_ringpostbuf_put(struct lpfc_hba *phba, struct lpfc_sli_ring *pring,
			 struct lpfc_dmabuf *mp)
{
	/* Stick struct lpfc_dmabuf at end of postbufq so driver can look it up
	   later */
	spin_lock_irq(&phba->hbalock);
	list_add_tail(&mp->list, &pring->postbufq);
	pring->postbufq_cnt++;
	spin_unlock_irq(&phba->hbalock);
	return 0;
}

uint32_t
lpfc_sli_get_buffer_tag(struct lpfc_hba *phba)
{
	spin_lock_irq(&phba->hbalock);
	phba->buffer_tag_count++;
	/*
	 * Always set the QUE_BUFTAG_BIT to distiguish between
	 * a tag assigned by HBQ.
	 */
	phba->buffer_tag_count |= QUE_BUFTAG_BIT;
	spin_unlock_irq(&phba->hbalock);
	return phba->buffer_tag_count;
}

struct lpfc_dmabuf *
lpfc_sli_ring_taggedbuf_get(struct lpfc_hba *phba, struct lpfc_sli_ring *pring,
			uint32_t tag)
{
	struct lpfc_dmabuf *mp, *next_mp;
	struct list_head *slp = &pring->postbufq;

	/* Search postbufq, from the begining, looking for a match on tag */
	spin_lock_irq(&phba->hbalock);
	list_for_each_entry_safe(mp, next_mp, &pring->postbufq, list) {
		if (mp->buffer_tag == tag) {
			list_del_init(&mp->list);
			pring->postbufq_cnt--;
			spin_unlock_irq(&phba->hbalock);
			return mp;
		}
	}

	spin_unlock_irq(&phba->hbalock);
	lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
			"0402 Cannot find virtual addr for buffer tag on "
			"ring %d Data x%lx x%p x%p x%x\n",
			pring->ringno, (unsigned long) tag,
			slp->next, slp->prev, pring->postbufq_cnt);

	return NULL;
}

struct lpfc_dmabuf *
lpfc_sli_ringpostbuf_get(struct lpfc_hba *phba, struct lpfc_sli_ring *pring,
			 dma_addr_t phys)
{
	struct lpfc_dmabuf *mp, *next_mp;
	struct list_head *slp = &pring->postbufq;

	/* Search postbufq, from the begining, looking for a match on phys */
	spin_lock_irq(&phba->hbalock);
	list_for_each_entry_safe(mp, next_mp, &pring->postbufq, list) {
		if (mp->phys == phys) {
			list_del_init(&mp->list);
			pring->postbufq_cnt--;
			spin_unlock_irq(&phba->hbalock);
			return mp;
		}
	}

	spin_unlock_irq(&phba->hbalock);
	lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
			"0410 Cannot find virtual addr for mapped buf on "
			"ring %d Data x%llx x%p x%p x%x\n",
			pring->ringno, (unsigned long long)phys,
			slp->next, slp->prev, pring->postbufq_cnt);
	return NULL;
}

static void
lpfc_sli_abort_els_cmpl(struct lpfc_hba *phba, struct lpfc_iocbq *cmdiocb,
			struct lpfc_iocbq *rspiocb)
{
	IOCB_t *irsp = &rspiocb->iocb;
	uint16_t abort_iotag, abort_context;
	struct lpfc_iocbq *abort_iocb;
	struct lpfc_sli_ring *pring = &phba->sli.ring[LPFC_ELS_RING];

	abort_iocb = NULL;

	if (irsp->ulpStatus) {
		abort_context = cmdiocb->iocb.un.acxri.abortContextTag;
		abort_iotag = cmdiocb->iocb.un.acxri.abortIoTag;

		spin_lock_irq(&phba->hbalock);
		if (phba->sli_rev < LPFC_SLI_REV4) {
			if (abort_iotag != 0 &&
				abort_iotag <= phba->sli.last_iotag)
				abort_iocb =
					phba->sli.iocbq_lookup[abort_iotag];
		} else
			/* For sli4 the abort_tag is the XRI,
			 * so the abort routine puts the iotag  of the iocb
			 * being aborted in the context field of the abort
			 * IOCB.
			 */
			abort_iocb = phba->sli.iocbq_lookup[abort_context];

		lpfc_printf_log(phba, KERN_INFO, LOG_ELS | LOG_SLI,
				"0327 Cannot abort els iocb %p "
				"with tag %x context %x, abort status %x, "
				"abort code %x\n",
				abort_iocb, abort_iotag, abort_context,
				irsp->ulpStatus, irsp->un.ulpWord[4]);

		/*
		 *  If the iocb is not found in Firmware queue the iocb
		 *  might have completed already. Do not free it again.
		 */
		if (irsp->ulpStatus == IOSTAT_LOCAL_REJECT) {
			if (irsp->un.ulpWord[4] != IOERR_NO_XRI) {
				spin_unlock_irq(&phba->hbalock);
				lpfc_sli_release_iocbq(phba, cmdiocb);
				return;
			}
			/* For SLI4 the ulpContext field for abort IOCB
			 * holds the iotag of the IOCB being aborted so
			 * the local abort_context needs to be reset to
			 * match the aborted IOCBs ulpContext.
			 */
			if (abort_iocb && phba->sli_rev == LPFC_SLI_REV4)
				abort_context = abort_iocb->iocb.ulpContext;
		}
		/*
		 * make sure we have the right iocbq before taking it
		 * off the txcmplq and try to call completion routine.
		 */
		if (!abort_iocb ||
		    abort_iocb->iocb.ulpContext != abort_context ||
		    (abort_iocb->iocb_flag & LPFC_DRIVER_ABORTED) == 0)
			spin_unlock_irq(&phba->hbalock);
		else if (phba->sli_rev < LPFC_SLI_REV4) {
			/*
			 * leave the SLI4 aborted command on the txcmplq
			 * list and the command complete WCQE's XB bit
			 * will tell whether the SGL (XRI) can be released
			 * immediately or to the aborted SGL list for the
			 * following abort XRI from the HBA.
			 */
			list_del_init(&abort_iocb->list);
			pring->txcmplq_cnt--;

			/* Firmware could still be in progress of DMAing
			 * payload, so don't free data buffer till after
			 * a hbeat.
			 */
			abort_iocb->iocb_flag |= LPFC_DELAY_MEM_FREE;
			abort_iocb->iocb_flag &= ~LPFC_DRIVER_ABORTED;
			spin_unlock_irq(&phba->hbalock);

			abort_iocb->iocb.ulpStatus = IOSTAT_LOCAL_REJECT;
			abort_iocb->iocb.un.ulpWord[4] = IOERR_ABORT_REQUESTED;
			(abort_iocb->iocb_cmpl)(phba, abort_iocb, abort_iocb);
		} else
			spin_unlock_irq(&phba->hbalock);
	}

	lpfc_sli_release_iocbq(phba, cmdiocb);
	return;
}

static void
lpfc_ignore_els_cmpl(struct lpfc_hba *phba, struct lpfc_iocbq *cmdiocb,
		     struct lpfc_iocbq *rspiocb)
{
	IOCB_t *irsp = &rspiocb->iocb;

	/* ELS cmd tag <ulpIoTag> completes */
	lpfc_printf_log(phba, KERN_INFO, LOG_ELS,
			"0139 Ignoring ELS cmd tag x%x completion Data: "
			"x%x x%x x%x\n",
			irsp->ulpIoTag, irsp->ulpStatus,
			irsp->un.ulpWord[4], irsp->ulpTimeout);
	if (cmdiocb->iocb.ulpCommand == CMD_GEN_REQUEST64_CR)
		lpfc_ct_free_iocb(phba, cmdiocb);
	else
		lpfc_els_free_iocb(phba, cmdiocb);
	return;
}

int
lpfc_sli_issue_abort_iotag(struct lpfc_hba *phba, struct lpfc_sli_ring *pring,
			   struct lpfc_iocbq *cmdiocb)
{
	struct lpfc_vport *vport = cmdiocb->vport;
	struct lpfc_iocbq *abtsiocbp;
	IOCB_t *icmd = NULL;
	IOCB_t *iabt = NULL;
	int retval = IOCB_ERROR;

	/*
	 * There are certain command types we don't want to abort.  And we
	 * don't want to abort commands that are already in the process of
	 * being aborted.
	 */
	icmd = &cmdiocb->iocb;
	if (icmd->ulpCommand == CMD_ABORT_XRI_CN ||
	    icmd->ulpCommand == CMD_CLOSE_XRI_CN ||
	    (cmdiocb->iocb_flag & LPFC_DRIVER_ABORTED) != 0)
		return 0;

	/* If we're unloading, don't abort iocb on the ELS ring, but change the
	 * callback so that nothing happens when it finishes.
	 */
	if ((vport->load_flag & FC_UNLOADING) &&
	    (pring->ringno == LPFC_ELS_RING)) {
		if (cmdiocb->iocb_flag & LPFC_IO_FABRIC)
			cmdiocb->fabric_iocb_cmpl = lpfc_ignore_els_cmpl;
		else
			cmdiocb->iocb_cmpl = lpfc_ignore_els_cmpl;
		goto abort_iotag_exit;
	}

	/* issue ABTS for this IOCB based on iotag */
	abtsiocbp = __lpfc_sli_get_iocbq(phba);
	if (abtsiocbp == NULL)
		return 0;

	/* This signals the response to set the correct status
	 * before calling the completion handler
	 */
	cmdiocb->iocb_flag |= LPFC_DRIVER_ABORTED;

	iabt = &abtsiocbp->iocb;
	iabt->un.acxri.abortType = ABORT_TYPE_ABTS;
	iabt->un.acxri.abortContextTag = icmd->ulpContext;
	if (phba->sli_rev == LPFC_SLI_REV4) {
		iabt->un.acxri.abortIoTag = cmdiocb->sli4_xritag;
		iabt->un.acxri.abortContextTag = cmdiocb->iotag;
	}
	else
		iabt->un.acxri.abortIoTag = icmd->ulpIoTag;
	iabt->ulpLe = 1;
	iabt->ulpClass = icmd->ulpClass;

	/* ABTS WQE must go to the same WQ as the WQE to be aborted */
	abtsiocbp->fcp_wqidx = cmdiocb->fcp_wqidx;
	if (cmdiocb->iocb_flag & LPFC_IO_FCP)
		abtsiocbp->iocb_flag |= LPFC_USE_FCPWQIDX;

	if (phba->link_state >= LPFC_LINK_UP)
		iabt->ulpCommand = CMD_ABORT_XRI_CN;
	else
		iabt->ulpCommand = CMD_CLOSE_XRI_CN;

	abtsiocbp->iocb_cmpl = lpfc_sli_abort_els_cmpl;

	lpfc_printf_vlog(vport, KERN_INFO, LOG_SLI,
			 "0339 Abort xri x%x, original iotag x%x, "
			 "abort cmd iotag x%x\n",
			 iabt->un.acxri.abortContextTag,
			 iabt->un.acxri.abortIoTag, abtsiocbp->iotag);
	retval = __lpfc_sli_issue_iocb(phba, pring->ringno, abtsiocbp, 0);

	if (retval)
		__lpfc_sli_release_iocbq(phba, abtsiocbp);
abort_iotag_exit:
	/*
	 * Caller to this routine should check for IOCB_ERROR
	 * and handle it properly.  This routine no longer removes
	 * iocb off txcmplq and call compl in case of IOCB_ERROR.
	 */
	return retval;
}

static int
lpfc_sli_validate_fcp_iocb(struct lpfc_iocbq *iocbq, struct lpfc_vport *vport,
			   uint16_t tgt_id, uint64_t lun_id,
			   lpfc_ctx_cmd ctx_cmd)
{
	struct lpfc_scsi_buf *lpfc_cmd;
	int rc = 1;

	if (!(iocbq->iocb_flag &  LPFC_IO_FCP))
		return rc;

	if (iocbq->vport != vport)
		return rc;

	lpfc_cmd = container_of(iocbq, struct lpfc_scsi_buf, cur_iocbq);

	if (lpfc_cmd->pCmd == NULL)
		return rc;

	switch (ctx_cmd) {
	case LPFC_CTX_LUN:
		if ((lpfc_cmd->rdata->pnode) &&
		    (lpfc_cmd->rdata->pnode->nlp_sid == tgt_id) &&
		    (scsilun_to_int(&lpfc_cmd->fcp_cmnd->fcp_lun) == lun_id))
			rc = 0;
		break;
	case LPFC_CTX_TGT:
		if ((lpfc_cmd->rdata->pnode) &&
		    (lpfc_cmd->rdata->pnode->nlp_sid == tgt_id))
			rc = 0;
		break;
	case LPFC_CTX_HOST:
		rc = 0;
		break;
	default:
		printk(KERN_ERR "%s: Unknown context cmd type, value %d\n",
			__func__, ctx_cmd);
		break;
	}

	return rc;
}

int
lpfc_sli_sum_iocb(struct lpfc_vport *vport, uint16_t tgt_id, uint64_t lun_id,
		  lpfc_ctx_cmd ctx_cmd)
{
	struct lpfc_hba *phba = vport->phba;
	struct lpfc_iocbq *iocbq;
	int sum, i;

	for (i = 1, sum = 0; i <= phba->sli.last_iotag; i++) {
		iocbq = phba->sli.iocbq_lookup[i];

		if (lpfc_sli_validate_fcp_iocb (iocbq, vport, tgt_id, lun_id,
						ctx_cmd) == 0)
			sum++;
	}

	return sum;
}

void
lpfc_sli_abort_fcp_cmpl(struct lpfc_hba *phba, struct lpfc_iocbq *cmdiocb,
			struct lpfc_iocbq *rspiocb)
{
	lpfc_sli_release_iocbq(phba, cmdiocb);
	return;
}

int
lpfc_sli_abort_iocb(struct lpfc_vport *vport, struct lpfc_sli_ring *pring,
		    uint16_t tgt_id, uint64_t lun_id, lpfc_ctx_cmd abort_cmd)
{
	struct lpfc_hba *phba = vport->phba;
	struct lpfc_iocbq *iocbq;
	struct lpfc_iocbq *abtsiocb;
	IOCB_t *cmd = NULL;
	int errcnt = 0, ret_val = 0;
	int i;

	for (i = 1; i <= phba->sli.last_iotag; i++) {
		iocbq = phba->sli.iocbq_lookup[i];

		if (lpfc_sli_validate_fcp_iocb(iocbq, vport, tgt_id, lun_id,
					       abort_cmd) != 0)
			continue;

		/* issue ABTS for this IOCB based on iotag */
		abtsiocb = lpfc_sli_get_iocbq(phba);
		if (abtsiocb == NULL) {
			errcnt++;
			continue;
		}

		cmd = &iocbq->iocb;
		abtsiocb->iocb.un.acxri.abortType = ABORT_TYPE_ABTS;
		abtsiocb->iocb.un.acxri.abortContextTag = cmd->ulpContext;
		if (phba->sli_rev == LPFC_SLI_REV4)
			abtsiocb->iocb.un.acxri.abortIoTag = iocbq->sli4_xritag;
		else
			abtsiocb->iocb.un.acxri.abortIoTag = cmd->ulpIoTag;
		abtsiocb->iocb.ulpLe = 1;
		abtsiocb->iocb.ulpClass = cmd->ulpClass;
		abtsiocb->vport = phba->pport;

		/* ABTS WQE must go to the same WQ as the WQE to be aborted */
		abtsiocb->fcp_wqidx = iocbq->fcp_wqidx;
		if (iocbq->iocb_flag & LPFC_IO_FCP)
			abtsiocb->iocb_flag |= LPFC_USE_FCPWQIDX;

		if (lpfc_is_link_up(phba))
			abtsiocb->iocb.ulpCommand = CMD_ABORT_XRI_CN;
		else
			abtsiocb->iocb.ulpCommand = CMD_CLOSE_XRI_CN;

		/* Setup callback routine and issue the command. */
		abtsiocb->iocb_cmpl = lpfc_sli_abort_fcp_cmpl;
		ret_val = lpfc_sli_issue_iocb(phba, pring->ringno,
					      abtsiocb, 0);
		if (ret_val == IOCB_ERROR) {
			lpfc_sli_release_iocbq(phba, abtsiocb);
			errcnt++;
			continue;
		}
	}

	return errcnt;
}

static void
lpfc_sli_wake_iocb_wait(struct lpfc_hba *phba,
			struct lpfc_iocbq *cmdiocbq,
			struct lpfc_iocbq *rspiocbq)
{
	wait_queue_head_t *pdone_q;
	unsigned long iflags;
	struct lpfc_scsi_buf *lpfc_cmd;

	spin_lock_irqsave(&phba->hbalock, iflags);
	cmdiocbq->iocb_flag |= LPFC_IO_WAKE;
	if (cmdiocbq->context2 && rspiocbq)
		memcpy(&((struct lpfc_iocbq *)cmdiocbq->context2)->iocb,
		       &rspiocbq->iocb, sizeof(IOCB_t));

	/* Set the exchange busy flag for task management commands */
	if ((cmdiocbq->iocb_flag & LPFC_IO_FCP) &&
		!(cmdiocbq->iocb_flag & LPFC_IO_LIBDFC)) {
		lpfc_cmd = container_of(cmdiocbq, struct lpfc_scsi_buf,
			cur_iocbq);
		lpfc_cmd->exch_busy = rspiocbq->iocb_flag & LPFC_EXCHANGE_BUSY;
	}

	pdone_q = cmdiocbq->context_un.wait_queue;
	if (pdone_q)
		wake_up(pdone_q);
	spin_unlock_irqrestore(&phba->hbalock, iflags);
	return;
}

static int
lpfc_chk_iocb_flg(struct lpfc_hba *phba,
		 struct lpfc_iocbq *piocbq, uint32_t flag)
{
	unsigned long iflags;
	int ret;

	spin_lock_irqsave(&phba->hbalock, iflags);
	ret = piocbq->iocb_flag & flag;
	spin_unlock_irqrestore(&phba->hbalock, iflags);
	return ret;

}

int
lpfc_sli_issue_iocb_wait(struct lpfc_hba *phba,
			 uint32_t ring_number,
			 struct lpfc_iocbq *piocb,
			 struct lpfc_iocbq *prspiocbq,
			 uint32_t timeout)
{
	DECLARE_WAIT_QUEUE_HEAD_ONSTACK(done_q);
	long timeleft, timeout_req = 0;
	int retval = IOCB_SUCCESS;
	uint32_t creg_val;

	/*
	 * If the caller has provided a response iocbq buffer, then context2
	 * is NULL or its an error.
	 */
	if (prspiocbq) {
		if (piocb->context2)
			return IOCB_ERROR;
		piocb->context2 = prspiocbq;
	}

	piocb->iocb_cmpl = lpfc_sli_wake_iocb_wait;
	piocb->context_un.wait_queue = &done_q;
	piocb->iocb_flag &= ~LPFC_IO_WAKE;

	if (phba->cfg_poll & DISABLE_FCP_RING_INT) {
		creg_val = readl(phba->HCregaddr);
		creg_val |= (HC_R0INT_ENA << LPFC_FCP_RING);
		writel(creg_val, phba->HCregaddr);
		readl(phba->HCregaddr); /* flush */
	}

	retval = lpfc_sli_issue_iocb(phba, ring_number, piocb, 0);
	if (retval == IOCB_SUCCESS) {
		timeout_req = timeout * HZ;
		timeleft = wait_event_timeout(done_q,
				lpfc_chk_iocb_flg(phba, piocb, LPFC_IO_WAKE),
				timeout_req);

		if (piocb->iocb_flag & LPFC_IO_WAKE) {
			lpfc_printf_log(phba, KERN_INFO, LOG_SLI,
					"0331 IOCB wake signaled\n");
		} else if (timeleft == 0) {
			lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
					"0338 IOCB wait timeout error - no "
					"wake response Data x%x\n", timeout);
			retval = IOCB_TIMEDOUT;
		} else {
			lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
					"0330 IOCB wake NOT set, "
					"Data x%x x%lx\n",
					timeout, (timeleft / jiffies));
			retval = IOCB_TIMEDOUT;
		}
	} else {
		lpfc_printf_log(phba, KERN_INFO, LOG_SLI,
				"0332 IOCB wait issue failed, Data x%x\n",
				retval);
		retval = IOCB_ERROR;
	}

	if (phba->cfg_poll & DISABLE_FCP_RING_INT) {
		creg_val = readl(phba->HCregaddr);
		creg_val &= ~(HC_R0INT_ENA << LPFC_FCP_RING);
		writel(creg_val, phba->HCregaddr);
		readl(phba->HCregaddr); /* flush */
	}

	if (prspiocbq)
		piocb->context2 = NULL;

	piocb->context_un.wait_queue = NULL;
	piocb->iocb_cmpl = NULL;
	return retval;
}

int
lpfc_sli_issue_mbox_wait(struct lpfc_hba *phba, LPFC_MBOXQ_t *pmboxq,
			 uint32_t timeout)
{
	DECLARE_WAIT_QUEUE_HEAD_ONSTACK(done_q);
	int retval;
	unsigned long flag;

	/* The caller must leave context1 empty. */
	if (pmboxq->context1)
		return MBX_NOT_FINISHED;

	pmboxq->mbox_flag &= ~LPFC_MBX_WAKE;
	/* setup wake call as IOCB callback */
	pmboxq->mbox_cmpl = lpfc_sli_wake_mbox_wait;
	/* setup context field to pass wait_queue pointer to wake function  */
	pmboxq->context1 = &done_q;

	/* now issue the command */
	retval = lpfc_sli_issue_mbox(phba, pmboxq, MBX_NOWAIT);

	if (retval == MBX_BUSY || retval == MBX_SUCCESS) {
		wait_event_interruptible_timeout(done_q,
				pmboxq->mbox_flag & LPFC_MBX_WAKE,
				timeout * HZ);

		spin_lock_irqsave(&phba->hbalock, flag);
		pmboxq->context1 = NULL;
		/*
		 * if LPFC_MBX_WAKE flag is set the mailbox is completed
		 * else do not free the resources.
		 */
		if (pmboxq->mbox_flag & LPFC_MBX_WAKE)
			retval = MBX_SUCCESS;
		else {
			retval = MBX_TIMEOUT;
			pmboxq->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
		}
		spin_unlock_irqrestore(&phba->hbalock, flag);
	}

	return retval;
}

void
lpfc_sli_mbox_sys_shutdown(struct lpfc_hba *phba)
{
	struct lpfc_sli *psli = &phba->sli;
	uint8_t actcmd = MBX_HEARTBEAT;
	unsigned long timeout;

	spin_lock_irq(&phba->hbalock);
	psli->sli_flag |= LPFC_SLI_ASYNC_MBX_BLK;
	spin_unlock_irq(&phba->hbalock);

	if (psli->sli_flag & LPFC_SLI_ACTIVE) {
		spin_lock_irq(&phba->hbalock);
		if (phba->sli.mbox_active)
			actcmd = phba->sli.mbox_active->u.mb.mbxCommand;
		spin_unlock_irq(&phba->hbalock);
		/* Determine how long we might wait for the active mailbox
		 * command to be gracefully completed by firmware.
		 */
		timeout = msecs_to_jiffies(lpfc_mbox_tmo_val(phba, actcmd) *
					   1000) + jiffies;
		while (phba->sli.mbox_active) {
			/* Check active mailbox complete status every 2ms */
			msleep(2);
			if (time_after(jiffies, timeout))
				/* Timeout, let the mailbox flush routine to
				 * forcefully release active mailbox command
				 */
				break;
		}
	}
	lpfc_sli_mbox_sys_flush(phba);
}

static int
lpfc_sli_eratt_read(struct lpfc_hba *phba)
{
	uint32_t ha_copy;

	/* Read chip Host Attention (HA) register */
	ha_copy = readl(phba->HAregaddr);
	if (ha_copy & HA_ERATT) {
		/* Read host status register to retrieve error event */
		lpfc_sli_read_hs(phba);

		/* Check if there is a deferred error condition is active */
		if ((HS_FFER1 & phba->work_hs) &&
		    ((HS_FFER2 | HS_FFER3 | HS_FFER4 | HS_FFER5 |
		     HS_FFER6 | HS_FFER7) & phba->work_hs)) {
			phba->hba_flag |= DEFER_ERATT;
			/* Clear all interrupt enable conditions */
			writel(0, phba->HCregaddr);
			readl(phba->HCregaddr);
		}

		/* Set the driver HA work bitmap */
		phba->work_ha |= HA_ERATT;
		/* Indicate polling handles this ERATT */
		phba->hba_flag |= HBA_ERATT_HANDLED;
		return 1;
	}
	return 0;
}

static int
lpfc_sli4_eratt_read(struct lpfc_hba *phba)
{
	uint32_t uerr_sta_hi, uerr_sta_lo;

	/* For now, use the SLI4 device internal unrecoverable error
	 * registers for error attention. This can be changed later.
	 */
	uerr_sta_lo = readl(phba->sli4_hba.UERRLOregaddr);
	uerr_sta_hi = readl(phba->sli4_hba.UERRHIregaddr);
	if ((~phba->sli4_hba.ue_mask_lo & uerr_sta_lo) ||
	    (~phba->sli4_hba.ue_mask_hi & uerr_sta_hi)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"1423 HBA Unrecoverable error: "
				"uerr_lo_reg=0x%x, uerr_hi_reg=0x%x, "
				"ue_mask_lo_reg=0x%x, ue_mask_hi_reg=0x%x\n",
				uerr_sta_lo, uerr_sta_hi,
				phba->sli4_hba.ue_mask_lo,
				phba->sli4_hba.ue_mask_hi);
		phba->work_status[0] = uerr_sta_lo;
		phba->work_status[1] = uerr_sta_hi;
		/* Set the driver HA work bitmap */
		phba->work_ha |= HA_ERATT;
		/* Indicate polling handles this ERATT */
		phba->hba_flag |= HBA_ERATT_HANDLED;
		return 1;
	}
	return 0;
}

int
lpfc_sli_check_eratt(struct lpfc_hba *phba)
{
	uint32_t ha_copy;

	/* If somebody is waiting to handle an eratt, don't process it
	 * here. The brdkill function will do this.
	 */
	if (phba->link_flag & LS_IGNORE_ERATT)
		return 0;

	/* Check if interrupt handler handles this ERATT */
	spin_lock_irq(&phba->hbalock);
	if (phba->hba_flag & HBA_ERATT_HANDLED) {
		/* Interrupt handler has handled ERATT */
		spin_unlock_irq(&phba->hbalock);
		return 0;
	}

	/*
	 * If there is deferred error attention, do not check for error
	 * attention
	 */
	if (unlikely(phba->hba_flag & DEFER_ERATT)) {
		spin_unlock_irq(&phba->hbalock);
		return 0;
	}

	/* If PCI channel is offline, don't process it */
	if (unlikely(pci_channel_offline(phba->pcidev))) {
		spin_unlock_irq(&phba->hbalock);
		return 0;
	}

	switch (phba->sli_rev) {
	case LPFC_SLI_REV2:
	case LPFC_SLI_REV3:
		/* Read chip Host Attention (HA) register */
		ha_copy = lpfc_sli_eratt_read(phba);
		break;
	case LPFC_SLI_REV4:
		/* Read devcie Uncoverable Error (UERR) registers */
		ha_copy = lpfc_sli4_eratt_read(phba);
		break;
	default:
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"0299 Invalid SLI revision (%d)\n",
				phba->sli_rev);
		ha_copy = 0;
		break;
	}
	spin_unlock_irq(&phba->hbalock);

	return ha_copy;
}

static inline int
lpfc_intr_state_check(struct lpfc_hba *phba)
{
	/* If the pci channel is offline, ignore all the interrupts */
	if (unlikely(pci_channel_offline(phba->pcidev)))
		return -EIO;

	/* Update device level interrupt statistics */
	phba->sli.slistat.sli_intr++;

	/* Ignore all interrupts during initialization. */
	if (unlikely(phba->link_state < LPFC_LINK_DOWN))
		return -EIO;

	return 0;
}

irqreturn_t
lpfc_sli_sp_intr_handler(int irq, void *dev_id)
{
	struct lpfc_hba  *phba;
	uint32_t ha_copy, hc_copy;
	uint32_t work_ha_copy;
	unsigned long status;
	unsigned long iflag;
	uint32_t control;

	MAILBOX_t *mbox, *pmbox;
	struct lpfc_vport *vport;
	struct lpfc_nodelist *ndlp;
	struct lpfc_dmabuf *mp;
	LPFC_MBOXQ_t *pmb;
	int rc;

	/*
	 * Get the driver's phba structure from the dev_id and
	 * assume the HBA is not interrupting.
	 */
	phba = (struct lpfc_hba *)dev_id;

	if (unlikely(!phba))
		return IRQ_NONE;

	/*
	 * Stuff needs to be attented to when this function is invoked as an
	 * individual interrupt handler in MSI-X multi-message interrupt mode
	 */
	if (phba->intr_type == MSIX) {
		/* Check device state for handling interrupt */
		if (lpfc_intr_state_check(phba))
			return IRQ_NONE;
		/* Need to read HA REG for slow-path events */
		spin_lock_irqsave(&phba->hbalock, iflag);
		ha_copy = readl(phba->HAregaddr);
		/* If somebody is waiting to handle an eratt don't process it
		 * here. The brdkill function will do this.
		 */
		if (phba->link_flag & LS_IGNORE_ERATT)
			ha_copy &= ~HA_ERATT;
		/* Check the need for handling ERATT in interrupt handler */
		if (ha_copy & HA_ERATT) {
			if (phba->hba_flag & HBA_ERATT_HANDLED)
				/* ERATT polling has handled ERATT */
				ha_copy &= ~HA_ERATT;
			else
				/* Indicate interrupt handler handles ERATT */
				phba->hba_flag |= HBA_ERATT_HANDLED;
		}

		/*
		 * If there is deferred error attention, do not check for any
		 * interrupt.
		 */
		if (unlikely(phba->hba_flag & DEFER_ERATT)) {
			spin_unlock_irqrestore(&phba->hbalock, iflag);
			return IRQ_NONE;
		}

		/* Clear up only attention source related to slow-path */
		hc_copy = readl(phba->HCregaddr);
		writel(hc_copy & ~(HC_MBINT_ENA | HC_R2INT_ENA |
			HC_LAINT_ENA | HC_ERINT_ENA),
			phba->HCregaddr);
		writel((ha_copy & (HA_MBATT | HA_R2_CLR_MSK)),
			phba->HAregaddr);
		writel(hc_copy, phba->HCregaddr);
		readl(phba->HAregaddr); /* flush */
		spin_unlock_irqrestore(&phba->hbalock, iflag);
	} else
		ha_copy = phba->ha_copy;

	work_ha_copy = ha_copy & phba->work_ha_mask;

	if (work_ha_copy) {
		if (work_ha_copy & HA_LATT) {
			if (phba->sli.sli_flag & LPFC_PROCESS_LA) {
				/*
				 * Turn off Link Attention interrupts
				 * until CLEAR_LA done
				 */
				spin_lock_irqsave(&phba->hbalock, iflag);
				phba->sli.sli_flag &= ~LPFC_PROCESS_LA;
				control = readl(phba->HCregaddr);
				control &= ~HC_LAINT_ENA;
				writel(control, phba->HCregaddr);
				readl(phba->HCregaddr); /* flush */
				spin_unlock_irqrestore(&phba->hbalock, iflag);
			}
			else
				work_ha_copy &= ~HA_LATT;
		}

		if (work_ha_copy & ~(HA_ERATT | HA_MBATT | HA_LATT)) {
			/*
			 * Turn off Slow Rings interrupts, LPFC_ELS_RING is
			 * the only slow ring.
			 */
			status = (work_ha_copy &
				(HA_RXMASK  << (4*LPFC_ELS_RING)));
			status >>= (4*LPFC_ELS_RING);
			if (status & HA_RXMASK) {
				spin_lock_irqsave(&phba->hbalock, iflag);
				control = readl(phba->HCregaddr);

				lpfc_debugfs_slow_ring_trc(phba,
				"ISR slow ring:   ctl:x%x stat:x%x isrcnt:x%x",
				control, status,
				(uint32_t)phba->sli.slistat.sli_intr);

				if (control & (HC_R0INT_ENA << LPFC_ELS_RING)) {
					lpfc_debugfs_slow_ring_trc(phba,
						"ISR Disable ring:"
						"pwork:x%x hawork:x%x wait:x%x",
						phba->work_ha, work_ha_copy,
						(uint32_t)((unsigned long)
						&phba->work_waitq));

					control &=
					    ~(HC_R0INT_ENA << LPFC_ELS_RING);
					writel(control, phba->HCregaddr);
					readl(phba->HCregaddr); /* flush */
				}
				else {
					lpfc_debugfs_slow_ring_trc(phba,
						"ISR slow ring:   pwork:"
						"x%x hawork:x%x wait:x%x",
						phba->work_ha, work_ha_copy,
						(uint32_t)((unsigned long)
						&phba->work_waitq));
				}
				spin_unlock_irqrestore(&phba->hbalock, iflag);
			}
		}
		spin_lock_irqsave(&phba->hbalock, iflag);
		if (work_ha_copy & HA_ERATT) {
			lpfc_sli_read_hs(phba);
			/*
			 * Check if there is a deferred error condition
			 * is active
			 */
			if ((HS_FFER1 & phba->work_hs) &&
				((HS_FFER2 | HS_FFER3 | HS_FFER4 | HS_FFER5 |
				HS_FFER6 | HS_FFER7) & phba->work_hs)) {
				phba->hba_flag |= DEFER_ERATT;
				/* Clear all interrupt enable conditions */
				writel(0, phba->HCregaddr);
				readl(phba->HCregaddr);
			}
		}

		if ((work_ha_copy & HA_MBATT) && (phba->sli.mbox_active)) {
			pmb = phba->sli.mbox_active;
			pmbox = &pmb->u.mb;
			mbox = phba->mbox;
			vport = pmb->vport;

			/* First check out the status word */
			lpfc_sli_pcimem_bcopy(mbox, pmbox, sizeof(uint32_t));
			if (pmbox->mbxOwner != OWN_HOST) {
				spin_unlock_irqrestore(&phba->hbalock, iflag);
				/*
				 * Stray Mailbox Interrupt, mbxCommand <cmd>
				 * mbxStatus <status>
				 */
				lpfc_printf_log(phba, KERN_ERR, LOG_MBOX |
						LOG_SLI,
						"(%d):0304 Stray Mailbox "
						"Interrupt mbxCommand x%x "
						"mbxStatus x%x\n",
						(vport ? vport->vpi : 0),
						pmbox->mbxCommand,
						pmbox->mbxStatus);
				/* clear mailbox attention bit */
				work_ha_copy &= ~HA_MBATT;
			} else {
				phba->sli.mbox_active = NULL;
				spin_unlock_irqrestore(&phba->hbalock, iflag);
				phba->last_completion_time = jiffies;
				del_timer(&phba->sli.mbox_tmo);
				if (pmb->mbox_cmpl) {
					lpfc_sli_pcimem_bcopy(mbox, pmbox,
							MAILBOX_CMD_SIZE);
					if (pmb->out_ext_byte_len &&
						pmb->context2)
						lpfc_sli_pcimem_bcopy(
						phba->mbox_ext,
						pmb->context2,
						pmb->out_ext_byte_len);
				}
				if (pmb->mbox_flag & LPFC_MBX_IMED_UNREG) {
					pmb->mbox_flag &= ~LPFC_MBX_IMED_UNREG;

					lpfc_debugfs_disc_trc(vport,
						LPFC_DISC_TRC_MBOX_VPORT,
						"MBOX dflt rpi: : "
						"status:x%x rpi:x%x",
						(uint32_t)pmbox->mbxStatus,
						pmbox->un.varWords[0], 0);

					if (!pmbox->mbxStatus) {
						mp = (struct lpfc_dmabuf *)
							(pmb->context1);
						ndlp = (struct lpfc_nodelist *)
							pmb->context2;

						/* Reg_LOGIN of dflt RPI was
						 * successful. new lets get
						 * rid of the RPI using the
						 * same mbox buffer.
						 */
						lpfc_unreg_login(phba,
							vport->vpi,
							pmbox->un.varWords[0],
							pmb);
						pmb->mbox_cmpl =
							lpfc_mbx_cmpl_dflt_rpi;
						pmb->context1 = mp;
						pmb->context2 = ndlp;
						pmb->vport = vport;
						rc = lpfc_sli_issue_mbox(phba,
								pmb,
								MBX_NOWAIT);
						if (rc != MBX_BUSY)
							lpfc_printf_log(phba,
							KERN_ERR,
							LOG_MBOX | LOG_SLI,
							"0350 rc should have"
							"been MBX_BUSY\n");
						if (rc != MBX_NOT_FINISHED)
							goto send_current_mbox;
					}
				}
				spin_lock_irqsave(
						&phba->pport->work_port_lock,
						iflag);
				phba->pport->work_port_events &=
					~WORKER_MBOX_TMO;
				spin_unlock_irqrestore(
						&phba->pport->work_port_lock,
						iflag);
				lpfc_mbox_cmpl_put(phba, pmb);
			}
		} else
			spin_unlock_irqrestore(&phba->hbalock, iflag);

		if ((work_ha_copy & HA_MBATT) &&
		    (phba->sli.mbox_active == NULL)) {
send_current_mbox:
			/* Process next mailbox command if there is one */
			do {
				rc = lpfc_sli_issue_mbox(phba, NULL,
							 MBX_NOWAIT);
			} while (rc == MBX_NOT_FINISHED);
			if (rc != MBX_SUCCESS)
				lpfc_printf_log(phba, KERN_ERR, LOG_MBOX |
						LOG_SLI, "0349 rc should be "
						"MBX_SUCCESS\n");
		}

		spin_lock_irqsave(&phba->hbalock, iflag);
		phba->work_ha |= work_ha_copy;
		spin_unlock_irqrestore(&phba->hbalock, iflag);
		lpfc_worker_wake_up(phba);
	}
	return IRQ_HANDLED;

} /* lpfc_sli_sp_intr_handler */

irqreturn_t
lpfc_sli_fp_intr_handler(int irq, void *dev_id)
{
	struct lpfc_hba  *phba;
	uint32_t ha_copy;
	unsigned long status;
	unsigned long iflag;

	/* Get the driver's phba structure from the dev_id and
	 * assume the HBA is not interrupting.
	 */
	phba = (struct lpfc_hba *) dev_id;

	if (unlikely(!phba))
		return IRQ_NONE;

	/*
	 * Stuff needs to be attented to when this function is invoked as an
	 * individual interrupt handler in MSI-X multi-message interrupt mode
	 */
	if (phba->intr_type == MSIX) {
		/* Check device state for handling interrupt */
		if (lpfc_intr_state_check(phba))
			return IRQ_NONE;
		/* Need to read HA REG for FCP ring and other ring events */
		ha_copy = readl(phba->HAregaddr);
		/* Clear up only attention source related to fast-path */
		spin_lock_irqsave(&phba->hbalock, iflag);
		/*
		 * If there is deferred error attention, do not check for
		 * any interrupt.
		 */
		if (unlikely(phba->hba_flag & DEFER_ERATT)) {
			spin_unlock_irqrestore(&phba->hbalock, iflag);
			return IRQ_NONE;
		}
		writel((ha_copy & (HA_R0_CLR_MSK | HA_R1_CLR_MSK)),
			phba->HAregaddr);
		readl(phba->HAregaddr); /* flush */
		spin_unlock_irqrestore(&phba->hbalock, iflag);
	} else
		ha_copy = phba->ha_copy;

	/*
	 * Process all events on FCP ring. Take the optimized path for FCP IO.
	 */
	ha_copy &= ~(phba->work_ha_mask);

	status = (ha_copy & (HA_RXMASK << (4*LPFC_FCP_RING)));
	status >>= (4*LPFC_FCP_RING);
	if (status & HA_RXMASK)
		lpfc_sli_handle_fast_ring_event(phba,
						&phba->sli.ring[LPFC_FCP_RING],
						status);

	if (phba->cfg_multi_ring_support == 2) {
		/*
		 * Process all events on extra ring. Take the optimized path
		 * for extra ring IO.
		 */
		status = (ha_copy & (HA_RXMASK << (4*LPFC_EXTRA_RING)));
		status >>= (4*LPFC_EXTRA_RING);
		if (status & HA_RXMASK) {
			lpfc_sli_handle_fast_ring_event(phba,
					&phba->sli.ring[LPFC_EXTRA_RING],
					status);
		}
	}
	return IRQ_HANDLED;
}  /* lpfc_sli_fp_intr_handler */

irqreturn_t
lpfc_sli_intr_handler(int irq, void *dev_id)
{
	struct lpfc_hba  *phba;
	irqreturn_t sp_irq_rc, fp_irq_rc;
	unsigned long status1, status2;
	uint32_t hc_copy;

	/*
	 * Get the driver's phba structure from the dev_id and
	 * assume the HBA is not interrupting.
	 */
	phba = (struct lpfc_hba *) dev_id;

	if (unlikely(!phba))
		return IRQ_NONE;

	/* Check device state for handling interrupt */
	if (lpfc_intr_state_check(phba))
		return IRQ_NONE;

	spin_lock(&phba->hbalock);
	phba->ha_copy = readl(phba->HAregaddr);
	if (unlikely(!phba->ha_copy)) {
		spin_unlock(&phba->hbalock);
		return IRQ_NONE;
	} else if (phba->ha_copy & HA_ERATT) {
		if (phba->hba_flag & HBA_ERATT_HANDLED)
			/* ERATT polling has handled ERATT */
			phba->ha_copy &= ~HA_ERATT;
		else
			/* Indicate interrupt handler handles ERATT */
			phba->hba_flag |= HBA_ERATT_HANDLED;
	}

	/*
	 * If there is deferred error attention, do not check for any interrupt.
	 */
	if (unlikely(phba->hba_flag & DEFER_ERATT)) {
		spin_unlock_irq(&phba->hbalock);
		return IRQ_NONE;
	}

	/* Clear attention sources except link and error attentions */
	hc_copy = readl(phba->HCregaddr);
	writel(hc_copy & ~(HC_MBINT_ENA | HC_R0INT_ENA | HC_R1INT_ENA
		| HC_R2INT_ENA | HC_LAINT_ENA | HC_ERINT_ENA),
		phba->HCregaddr);
	writel((phba->ha_copy & ~(HA_LATT | HA_ERATT)), phba->HAregaddr);
	writel(hc_copy, phba->HCregaddr);
	readl(phba->HAregaddr); /* flush */
	spin_unlock(&phba->hbalock);

	/*
	 * Invokes slow-path host attention interrupt handling as appropriate.
	 */

	/* status of events with mailbox and link attention */
	status1 = phba->ha_copy & (HA_MBATT | HA_LATT | HA_ERATT);

	/* status of events with ELS ring */
	status2 = (phba->ha_copy & (HA_RXMASK  << (4*LPFC_ELS_RING)));
	status2 >>= (4*LPFC_ELS_RING);

	if (status1 || (status2 & HA_RXMASK))
		sp_irq_rc = lpfc_sli_sp_intr_handler(irq, dev_id);
	else
		sp_irq_rc = IRQ_NONE;

	/*
	 * Invoke fast-path host attention interrupt handling as appropriate.
	 */

	/* status of events with FCP ring */
	status1 = (phba->ha_copy & (HA_RXMASK << (4*LPFC_FCP_RING)));
	status1 >>= (4*LPFC_FCP_RING);

	/* status of events with extra ring */
	if (phba->cfg_multi_ring_support == 2) {
		status2 = (phba->ha_copy & (HA_RXMASK << (4*LPFC_EXTRA_RING)));
		status2 >>= (4*LPFC_EXTRA_RING);
	} else
		status2 = 0;

	if ((status1 & HA_RXMASK) || (status2 & HA_RXMASK))
		fp_irq_rc = lpfc_sli_fp_intr_handler(irq, dev_id);
	else
		fp_irq_rc = IRQ_NONE;

	/* Return device-level interrupt handling status */
	return (sp_irq_rc == IRQ_HANDLED) ? sp_irq_rc : fp_irq_rc;
}  /* lpfc_sli_intr_handler */

void lpfc_sli4_fcp_xri_abort_event_proc(struct lpfc_hba *phba)
{
	struct lpfc_cq_event *cq_event;

	/* First, declare the fcp xri abort event has been handled */
	spin_lock_irq(&phba->hbalock);
	phba->hba_flag &= ~FCP_XRI_ABORT_EVENT;
	spin_unlock_irq(&phba->hbalock);
	/* Now, handle all the fcp xri abort events */
	while (!list_empty(&phba->sli4_hba.sp_fcp_xri_aborted_work_queue)) {
		/* Get the first event from the head of the event queue */
		spin_lock_irq(&phba->hbalock);
		list_remove_head(&phba->sli4_hba.sp_fcp_xri_aborted_work_queue,
				 cq_event, struct lpfc_cq_event, list);
		spin_unlock_irq(&phba->hbalock);
		/* Notify aborted XRI for FCP work queue */
		lpfc_sli4_fcp_xri_aborted(phba, &cq_event->cqe.wcqe_axri);
		/* Free the event processed back to the free pool */
		lpfc_sli4_cq_event_release(phba, cq_event);
	}
}

void lpfc_sli4_els_xri_abort_event_proc(struct lpfc_hba *phba)
{
	struct lpfc_cq_event *cq_event;

	/* First, declare the els xri abort event has been handled */
	spin_lock_irq(&phba->hbalock);
	phba->hba_flag &= ~ELS_XRI_ABORT_EVENT;
	spin_unlock_irq(&phba->hbalock);
	/* Now, handle all the els xri abort events */
	while (!list_empty(&phba->sli4_hba.sp_els_xri_aborted_work_queue)) {
		/* Get the first event from the head of the event queue */
		spin_lock_irq(&phba->hbalock);
		list_remove_head(&phba->sli4_hba.sp_els_xri_aborted_work_queue,
				 cq_event, struct lpfc_cq_event, list);
		spin_unlock_irq(&phba->hbalock);
		/* Notify aborted XRI for ELS work queue */
		lpfc_sli4_els_xri_aborted(phba, &cq_event->cqe.wcqe_axri);
		/* Free the event processed back to the free pool */
		lpfc_sli4_cq_event_release(phba, cq_event);
	}
}

static void
lpfc_sli4_iocb_param_transfer(struct lpfc_hba *phba,
			      struct lpfc_iocbq *pIocbIn,
			      struct lpfc_iocbq *pIocbOut,
			      struct lpfc_wcqe_complete *wcqe)
{
	unsigned long iflags;
	size_t offset = offsetof(struct lpfc_iocbq, iocb);

	memcpy((char *)pIocbIn + offset, (char *)pIocbOut + offset,
	       sizeof(struct lpfc_iocbq) - offset);
	/* Map WCQE parameters into irspiocb parameters */
	pIocbIn->iocb.ulpStatus = bf_get(lpfc_wcqe_c_status, wcqe);
	if (pIocbOut->iocb_flag & LPFC_IO_FCP)
		if (pIocbIn->iocb.ulpStatus == IOSTAT_FCP_RSP_ERROR)
			pIocbIn->iocb.un.fcpi.fcpi_parm =
					pIocbOut->iocb.un.fcpi.fcpi_parm -
					wcqe->total_data_placed;
		else
			pIocbIn->iocb.un.ulpWord[4] = wcqe->parameter;
	else {
		pIocbIn->iocb.un.ulpWord[4] = wcqe->parameter;
		pIocbIn->iocb.un.genreq64.bdl.bdeSize = wcqe->total_data_placed;
	}

	/* Pick up HBA exchange busy condition */
	if (bf_get(lpfc_wcqe_c_xb, wcqe)) {
		spin_lock_irqsave(&phba->hbalock, iflags);
		pIocbIn->iocb_flag |= LPFC_EXCHANGE_BUSY;
		spin_unlock_irqrestore(&phba->hbalock, iflags);
	}
}

static struct lpfc_iocbq *
lpfc_sli4_els_wcqe_to_rspiocbq(struct lpfc_hba *phba,
			       struct lpfc_iocbq *irspiocbq)
{
	struct lpfc_sli_ring *pring = &phba->sli.ring[LPFC_ELS_RING];
	struct lpfc_iocbq *cmdiocbq;
	struct lpfc_wcqe_complete *wcqe;
	unsigned long iflags;

	wcqe = &irspiocbq->cq_event.cqe.wcqe_cmpl;
	spin_lock_irqsave(&phba->hbalock, iflags);
	pring->stats.iocb_event++;
	/* Look up the ELS command IOCB and create pseudo response IOCB */
	cmdiocbq = lpfc_sli_iocbq_lookup_by_tag(phba, pring,
				bf_get(lpfc_wcqe_c_request_tag, wcqe));
	spin_unlock_irqrestore(&phba->hbalock, iflags);

	if (unlikely(!cmdiocbq)) {
		lpfc_printf_log(phba, KERN_WARNING, LOG_SLI,
				"0386 ELS complete with no corresponding "
				"cmdiocb: iotag (%d)\n",
				bf_get(lpfc_wcqe_c_request_tag, wcqe));
		lpfc_sli_release_iocbq(phba, irspiocbq);
		return NULL;
	}

	/* Fake the irspiocbq and copy necessary response information */
	lpfc_sli4_iocb_param_transfer(phba, irspiocbq, cmdiocbq, wcqe);

	return irspiocbq;
}

static bool
lpfc_sli4_sp_handle_async_event(struct lpfc_hba *phba, struct lpfc_mcqe *mcqe)
{
	struct lpfc_cq_event *cq_event;
	unsigned long iflags;

	lpfc_printf_log(phba, KERN_INFO, LOG_SLI,
			"0392 Async Event: word0:x%x, word1:x%x, "
			"word2:x%x, word3:x%x\n", mcqe->word0,
			mcqe->mcqe_tag0, mcqe->mcqe_tag1, mcqe->trailer);

	/* Allocate a new internal CQ_EVENT entry */
	cq_event = lpfc_sli4_cq_event_alloc(phba);
	if (!cq_event) {
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0394 Failed to allocate CQ_EVENT entry\n");
		return false;
	}

	/* Move the CQE into an asynchronous event entry */
	memcpy(&cq_event->cqe, mcqe, sizeof(struct lpfc_mcqe));
	spin_lock_irqsave(&phba->hbalock, iflags);
	list_add_tail(&cq_event->list, &phba->sli4_hba.sp_asynce_work_queue);
	/* Set the async event flag */
	phba->hba_flag |= ASYNC_EVENT;
	spin_unlock_irqrestore(&phba->hbalock, iflags);

	return true;
}

static bool
lpfc_sli4_sp_handle_mbox_event(struct lpfc_hba *phba, struct lpfc_mcqe *mcqe)
{
	uint32_t mcqe_status;
	MAILBOX_t *mbox, *pmbox;
	struct lpfc_mqe *mqe;
	struct lpfc_vport *vport;
	struct lpfc_nodelist *ndlp;
	struct lpfc_dmabuf *mp;
	unsigned long iflags;
	LPFC_MBOXQ_t *pmb;
	bool workposted = false;
	int rc;

	/* If not a mailbox complete MCQE, out by checking mailbox consume */
	if (!bf_get(lpfc_trailer_completed, mcqe))
		goto out_no_mqe_complete;

	/* Get the reference to the active mbox command */
	spin_lock_irqsave(&phba->hbalock, iflags);
	pmb = phba->sli.mbox_active;
	if (unlikely(!pmb)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_MBOX,
				"1832 No pending MBOX command to handle\n");
		spin_unlock_irqrestore(&phba->hbalock, iflags);
		goto out_no_mqe_complete;
	}
	spin_unlock_irqrestore(&phba->hbalock, iflags);
	mqe = &pmb->u.mqe;
	pmbox = (MAILBOX_t *)&pmb->u.mqe;
	mbox = phba->mbox;
	vport = pmb->vport;

	/* Reset heartbeat timer */
	phba->last_completion_time = jiffies;
	del_timer(&phba->sli.mbox_tmo);

	/* Move mbox data to caller's mailbox region, do endian swapping */
	if (pmb->mbox_cmpl && mbox)
		lpfc_sli_pcimem_bcopy(mbox, mqe, sizeof(struct lpfc_mqe));
	/* Set the mailbox status with SLI4 range 0x4000 */
	mcqe_status = bf_get(lpfc_mcqe_status, mcqe);
	if (mcqe_status != MB_CQE_STATUS_SUCCESS)
		bf_set(lpfc_mqe_status, mqe,
		       (LPFC_MBX_ERROR_RANGE | mcqe_status));

	if (pmb->mbox_flag & LPFC_MBX_IMED_UNREG) {
		pmb->mbox_flag &= ~LPFC_MBX_IMED_UNREG;
		lpfc_debugfs_disc_trc(vport, LPFC_DISC_TRC_MBOX_VPORT,
				      "MBOX dflt rpi: status:x%x rpi:x%x",
				      mcqe_status,
				      pmbox->un.varWords[0], 0);
		if (mcqe_status == MB_CQE_STATUS_SUCCESS) {
			mp = (struct lpfc_dmabuf *)(pmb->context1);
			ndlp = (struct lpfc_nodelist *)pmb->context2;
			/* Reg_LOGIN of dflt RPI was successful. Now lets get
			 * RID of the PPI using the same mbox buffer.
			 */
			lpfc_unreg_login(phba, vport->vpi,
					 pmbox->un.varWords[0], pmb);
			pmb->mbox_cmpl = lpfc_mbx_cmpl_dflt_rpi;
			pmb->context1 = mp;
			pmb->context2 = ndlp;
			pmb->vport = vport;
			rc = lpfc_sli_issue_mbox(phba, pmb, MBX_NOWAIT);
			if (rc != MBX_BUSY)
				lpfc_printf_log(phba, KERN_ERR, LOG_MBOX |
						LOG_SLI, "0385 rc should "
						"have been MBX_BUSY\n");
			if (rc != MBX_NOT_FINISHED)
				goto send_current_mbox;
		}
	}
	spin_lock_irqsave(&phba->pport->work_port_lock, iflags);
	phba->pport->work_port_events &= ~WORKER_MBOX_TMO;
	spin_unlock_irqrestore(&phba->pport->work_port_lock, iflags);

	/* There is mailbox completion work to do */
	spin_lock_irqsave(&phba->hbalock, iflags);
	__lpfc_mbox_cmpl_put(phba, pmb);
	phba->work_ha |= HA_MBATT;
	spin_unlock_irqrestore(&phba->hbalock, iflags);
	workposted = true;

send_current_mbox:
	spin_lock_irqsave(&phba->hbalock, iflags);
	/* Release the mailbox command posting token */
	phba->sli.sli_flag &= ~LPFC_SLI_MBOX_ACTIVE;
	/* Setting active mailbox pointer need to be in sync to flag clear */
	phba->sli.mbox_active = NULL;
	spin_unlock_irqrestore(&phba->hbalock, iflags);
	/* Wake up worker thread to post the next pending mailbox command */
	lpfc_worker_wake_up(phba);
out_no_mqe_complete:
	if (bf_get(lpfc_trailer_consumed, mcqe))
		lpfc_sli4_mq_release(phba->sli4_hba.mbx_wq);
	return workposted;
}

static bool
lpfc_sli4_sp_handle_mcqe(struct lpfc_hba *phba, struct lpfc_cqe *cqe)
{
	struct lpfc_mcqe mcqe;
	bool workposted;

	/* Copy the mailbox MCQE and convert endian order as needed */
	lpfc_sli_pcimem_bcopy(cqe, &mcqe, sizeof(struct lpfc_mcqe));

	/* Invoke the proper event handling routine */
	if (!bf_get(lpfc_trailer_async, &mcqe))
		workposted = lpfc_sli4_sp_handle_mbox_event(phba, &mcqe);
	else
		workposted = lpfc_sli4_sp_handle_async_event(phba, &mcqe);
	return workposted;
}

static bool
lpfc_sli4_sp_handle_els_wcqe(struct lpfc_hba *phba,
			     struct lpfc_wcqe_complete *wcqe)
{
	struct lpfc_iocbq *irspiocbq;
	unsigned long iflags;

	/* Get an irspiocbq for later ELS response processing use */
	irspiocbq = lpfc_sli_get_iocbq(phba);
	if (!irspiocbq) {
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0387 Failed to allocate an iocbq\n");
		return false;
	}

	/* Save off the slow-path queue event for work thread to process */
	memcpy(&irspiocbq->cq_event.cqe.wcqe_cmpl, wcqe, sizeof(*wcqe));
	spin_lock_irqsave(&phba->hbalock, iflags);
	list_add_tail(&irspiocbq->cq_event.list,
		      &phba->sli4_hba.sp_queue_event);
	phba->hba_flag |= HBA_SP_QUEUE_EVT;
	spin_unlock_irqrestore(&phba->hbalock, iflags);

	return true;
}

static void
lpfc_sli4_sp_handle_rel_wcqe(struct lpfc_hba *phba,
			     struct lpfc_wcqe_release *wcqe)
{
	/* Check for the slow-path ELS work queue */
	if (bf_get(lpfc_wcqe_r_wq_id, wcqe) == phba->sli4_hba.els_wq->queue_id)
		lpfc_sli4_wq_release(phba->sli4_hba.els_wq,
				     bf_get(lpfc_wcqe_r_wqe_index, wcqe));
	else
		lpfc_printf_log(phba, KERN_WARNING, LOG_SLI,
				"2579 Slow-path wqe consume event carries "
				"miss-matched qid: wcqe-qid=x%x, sp-qid=x%x\n",
				bf_get(lpfc_wcqe_r_wqe_index, wcqe),
				phba->sli4_hba.els_wq->queue_id);
}

static bool
lpfc_sli4_sp_handle_abort_xri_wcqe(struct lpfc_hba *phba,
				   struct lpfc_queue *cq,
				   struct sli4_wcqe_xri_aborted *wcqe)
{
	bool workposted = false;
	struct lpfc_cq_event *cq_event;
	unsigned long iflags;

	/* Allocate a new internal CQ_EVENT entry */
	cq_event = lpfc_sli4_cq_event_alloc(phba);
	if (!cq_event) {
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0602 Failed to allocate CQ_EVENT entry\n");
		return false;
	}

	/* Move the CQE into the proper xri abort event list */
	memcpy(&cq_event->cqe, wcqe, sizeof(struct sli4_wcqe_xri_aborted));
	switch (cq->subtype) {
	case LPFC_FCP:
		spin_lock_irqsave(&phba->hbalock, iflags);
		list_add_tail(&cq_event->list,
			      &phba->sli4_hba.sp_fcp_xri_aborted_work_queue);
		/* Set the fcp xri abort event flag */
		phba->hba_flag |= FCP_XRI_ABORT_EVENT;
		spin_unlock_irqrestore(&phba->hbalock, iflags);
		workposted = true;
		break;
	case LPFC_ELS:
		spin_lock_irqsave(&phba->hbalock, iflags);
		list_add_tail(&cq_event->list,
			      &phba->sli4_hba.sp_els_xri_aborted_work_queue);
		/* Set the els xri abort event flag */
		phba->hba_flag |= ELS_XRI_ABORT_EVENT;
		spin_unlock_irqrestore(&phba->hbalock, iflags);
		workposted = true;
		break;
	default:
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0603 Invalid work queue CQE subtype (x%x)\n",
				cq->subtype);
		workposted = false;
		break;
	}
	return workposted;
}

static bool
lpfc_sli4_sp_handle_rcqe(struct lpfc_hba *phba, struct lpfc_rcqe *rcqe)
{
	bool workposted = false;
	struct lpfc_queue *hrq = phba->sli4_hba.hdr_rq;
	struct lpfc_queue *drq = phba->sli4_hba.dat_rq;
	struct hbq_dmabuf *dma_buf;
	uint32_t status;
	unsigned long iflags;

	if (bf_get(lpfc_rcqe_rq_id, rcqe) != hrq->queue_id)
		goto out;

	status = bf_get(lpfc_rcqe_status, rcqe);
	switch (status) {
	case FC_STATUS_RQ_BUF_LEN_EXCEEDED:
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"2537 Receive Frame Truncated!!\n");
	case FC_STATUS_RQ_SUCCESS:
		lpfc_sli4_rq_release(hrq, drq);
		spin_lock_irqsave(&phba->hbalock, iflags);
		dma_buf = lpfc_sli_hbqbuf_get(&phba->hbqs[0].hbq_buffer_list);
		if (!dma_buf) {
			spin_unlock_irqrestore(&phba->hbalock, iflags);
			goto out;
		}
		memcpy(&dma_buf->cq_event.cqe.rcqe_cmpl, rcqe, sizeof(*rcqe));
		/* save off the frame for the word thread to process */
		list_add_tail(&dma_buf->cq_event.list,
			      &phba->sli4_hba.sp_queue_event);
		/* Frame received */
		phba->hba_flag |= HBA_SP_QUEUE_EVT;
		spin_unlock_irqrestore(&phba->hbalock, iflags);
		workposted = true;
		break;
	case FC_STATUS_INSUFF_BUF_NEED_BUF:
	case FC_STATUS_INSUFF_BUF_FRM_DISC:
		/* Post more buffers if possible */
		spin_lock_irqsave(&phba->hbalock, iflags);
		phba->hba_flag |= HBA_POST_RECEIVE_BUFFER;
		spin_unlock_irqrestore(&phba->hbalock, iflags);
		workposted = true;
		break;
	}
out:
	return workposted;
}

static bool
lpfc_sli4_sp_handle_cqe(struct lpfc_hba *phba, struct lpfc_queue *cq,
			 struct lpfc_cqe *cqe)
{
	struct lpfc_cqe cqevt;
	bool workposted = false;

	/* Copy the work queue CQE and convert endian order if needed */
	lpfc_sli_pcimem_bcopy(cqe, &cqevt, sizeof(struct lpfc_cqe));

	/* Check and process for different type of WCQE and dispatch */
	switch (bf_get(lpfc_cqe_code, &cqevt)) {
	case CQE_CODE_COMPL_WQE:
		/* Process the WQ/RQ complete event */
		workposted = lpfc_sli4_sp_handle_els_wcqe(phba,
				(struct lpfc_wcqe_complete *)&cqevt);
		break;
	case CQE_CODE_RELEASE_WQE:
		/* Process the WQ release event */
		lpfc_sli4_sp_handle_rel_wcqe(phba,
				(struct lpfc_wcqe_release *)&cqevt);
		break;
	case CQE_CODE_XRI_ABORTED:
		/* Process the WQ XRI abort event */
		workposted = lpfc_sli4_sp_handle_abort_xri_wcqe(phba, cq,
				(struct sli4_wcqe_xri_aborted *)&cqevt);
		break;
	case CQE_CODE_RECEIVE:
		/* Process the RQ event */
		workposted = lpfc_sli4_sp_handle_rcqe(phba,
				(struct lpfc_rcqe *)&cqevt);
		break;
	default:
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0388 Not a valid WCQE code: x%x\n",
				bf_get(lpfc_cqe_code, &cqevt));
		break;
	}
	return workposted;
}

static void
lpfc_sli4_sp_handle_eqe(struct lpfc_hba *phba, struct lpfc_eqe *eqe)
{
	struct lpfc_queue *cq = NULL, *childq, *speq;
	struct lpfc_cqe *cqe;
	bool workposted = false;
	int ecount = 0;
	uint16_t cqid;

	if (bf_get_le32(lpfc_eqe_major_code, eqe) != 0) {
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0359 Not a valid slow-path completion "
				"event: majorcode=x%x, minorcode=x%x\n",
				bf_get_le32(lpfc_eqe_major_code, eqe),
				bf_get_le32(lpfc_eqe_minor_code, eqe));
		return;
	}

	/* Get the reference to the corresponding CQ */
	cqid = bf_get_le32(lpfc_eqe_resource_id, eqe);

	/* Search for completion queue pointer matching this cqid */
	speq = phba->sli4_hba.sp_eq;
	list_for_each_entry(childq, &speq->child_list, list) {
		if (childq->queue_id == cqid) {
			cq = childq;
			break;
		}
	}
	if (unlikely(!cq)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0365 Slow-path CQ identifier (%d) does "
				"not exist\n", cqid);
		return;
	}

	/* Process all the entries to the CQ */
	switch (cq->type) {
	case LPFC_MCQ:
		while ((cqe = lpfc_sli4_cq_get(cq))) {
			workposted |= lpfc_sli4_sp_handle_mcqe(phba, cqe);
			if (!(++ecount % LPFC_GET_QE_REL_INT))
				lpfc_sli4_cq_release(cq, LPFC_QUEUE_NOARM);
		}
		break;
	case LPFC_WCQ:
		while ((cqe = lpfc_sli4_cq_get(cq))) {
			workposted |= lpfc_sli4_sp_handle_cqe(phba, cq, cqe);
			if (!(++ecount % LPFC_GET_QE_REL_INT))
				lpfc_sli4_cq_release(cq, LPFC_QUEUE_NOARM);
		}
		break;
	default:
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0370 Invalid completion queue type (%d)\n",
				cq->type);
		return;
	}

	/* Catch the no cq entry condition, log an error */
	if (unlikely(ecount == 0))
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0371 No entry from the CQ: identifier "
				"(x%x), type (%d)\n", cq->queue_id, cq->type);

	/* In any case, flash and re-arm the RCQ */
	lpfc_sli4_cq_release(cq, LPFC_QUEUE_REARM);

	/* wake up worker thread if there are works to be done */
	if (workposted)
		lpfc_worker_wake_up(phba);
}

static void
lpfc_sli4_fp_handle_fcp_wcqe(struct lpfc_hba *phba,
			     struct lpfc_wcqe_complete *wcqe)
{
	struct lpfc_sli_ring *pring = &phba->sli.ring[LPFC_FCP_RING];
	struct lpfc_iocbq *cmdiocbq;
	struct lpfc_iocbq irspiocbq;
	unsigned long iflags;

	spin_lock_irqsave(&phba->hbalock, iflags);
	pring->stats.iocb_event++;
	spin_unlock_irqrestore(&phba->hbalock, iflags);

	/* Check for response status */
	if (unlikely(bf_get(lpfc_wcqe_c_status, wcqe))) {
		/* If resource errors reported from HBA, reduce queue
		 * depth of the SCSI device.
		 */
		if ((bf_get(lpfc_wcqe_c_status, wcqe) ==
		     IOSTAT_LOCAL_REJECT) &&
		    (wcqe->parameter == IOERR_NO_RESOURCES)) {
			phba->lpfc_rampdown_queue_depth(phba);
		}
		/* Log the error status */
		lpfc_printf_log(phba, KERN_WARNING, LOG_SLI,
				"0373 FCP complete error: status=x%x, "
				"hw_status=x%x, total_data_specified=%d, "
				"parameter=x%x, word3=x%x\n",
				bf_get(lpfc_wcqe_c_status, wcqe),
				bf_get(lpfc_wcqe_c_hw_status, wcqe),
				wcqe->total_data_placed, wcqe->parameter,
				wcqe->word3);
	}

	/* Look up the FCP command IOCB and create pseudo response IOCB */
	spin_lock_irqsave(&phba->hbalock, iflags);
	cmdiocbq = lpfc_sli_iocbq_lookup_by_tag(phba, pring,
				bf_get(lpfc_wcqe_c_request_tag, wcqe));
	spin_unlock_irqrestore(&phba->hbalock, iflags);
	if (unlikely(!cmdiocbq)) {
		lpfc_printf_log(phba, KERN_WARNING, LOG_SLI,
				"0374 FCP complete with no corresponding "
				"cmdiocb: iotag (%d)\n",
				bf_get(lpfc_wcqe_c_request_tag, wcqe));
		return;
	}
	if (unlikely(!cmdiocbq->iocb_cmpl)) {
		lpfc_printf_log(phba, KERN_WARNING, LOG_SLI,
				"0375 FCP cmdiocb not callback function "
				"iotag: (%d)\n",
				bf_get(lpfc_wcqe_c_request_tag, wcqe));
		return;
	}

	/* Fake the irspiocb and copy necessary response information */
	lpfc_sli4_iocb_param_transfer(phba, &irspiocbq, cmdiocbq, wcqe);

	if (cmdiocbq->iocb_flag & LPFC_DRIVER_ABORTED) {
		spin_lock_irqsave(&phba->hbalock, iflags);
		cmdiocbq->iocb_flag &= ~LPFC_DRIVER_ABORTED;
		spin_unlock_irqrestore(&phba->hbalock, iflags);
	}

	/* Pass the cmd_iocb and the rsp state to the upper layer */
	(cmdiocbq->iocb_cmpl)(phba, cmdiocbq, &irspiocbq);
}

static void
lpfc_sli4_fp_handle_rel_wcqe(struct lpfc_hba *phba, struct lpfc_queue *cq,
			     struct lpfc_wcqe_release *wcqe)
{
	struct lpfc_queue *childwq;
	bool wqid_matched = false;
	uint16_t fcp_wqid;

	/* Check for fast-path FCP work queue release */
	fcp_wqid = bf_get(lpfc_wcqe_r_wq_id, wcqe);
	list_for_each_entry(childwq, &cq->child_list, list) {
		if (childwq->queue_id == fcp_wqid) {
			lpfc_sli4_wq_release(childwq,
					bf_get(lpfc_wcqe_r_wqe_index, wcqe));
			wqid_matched = true;
			break;
		}
	}
	/* Report warning log message if no match found */
	if (wqid_matched != true)
		lpfc_printf_log(phba, KERN_WARNING, LOG_SLI,
				"2580 Fast-path wqe consume event carries "
				"miss-matched qid: wcqe-qid=x%x\n", fcp_wqid);
}

static int
lpfc_sli4_fp_handle_wcqe(struct lpfc_hba *phba, struct lpfc_queue *cq,
			 struct lpfc_cqe *cqe)
{
	struct lpfc_wcqe_release wcqe;
	bool workposted = false;

	/* Copy the work queue CQE and convert endian order if needed */
	lpfc_sli_pcimem_bcopy(cqe, &wcqe, sizeof(struct lpfc_cqe));

	/* Check and process for different type of WCQE and dispatch */
	switch (bf_get(lpfc_wcqe_c_code, &wcqe)) {
	case CQE_CODE_COMPL_WQE:
		/* Process the WQ complete event */
		lpfc_sli4_fp_handle_fcp_wcqe(phba,
				(struct lpfc_wcqe_complete *)&wcqe);
		break;
	case CQE_CODE_RELEASE_WQE:
		/* Process the WQ release event */
		lpfc_sli4_fp_handle_rel_wcqe(phba, cq,
				(struct lpfc_wcqe_release *)&wcqe);
		break;
	case CQE_CODE_XRI_ABORTED:
		/* Process the WQ XRI abort event */
		workposted = lpfc_sli4_sp_handle_abort_xri_wcqe(phba, cq,
				(struct sli4_wcqe_xri_aborted *)&wcqe);
		break;
	default:
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0144 Not a valid WCQE code: x%x\n",
				bf_get(lpfc_wcqe_c_code, &wcqe));
		break;
	}
	return workposted;
}

static void
lpfc_sli4_fp_handle_eqe(struct lpfc_hba *phba, struct lpfc_eqe *eqe,
			uint32_t fcp_cqidx)
{
	struct lpfc_queue *cq;
	struct lpfc_cqe *cqe;
	bool workposted = false;
	uint16_t cqid;
	int ecount = 0;

	if (unlikely(bf_get_le32(lpfc_eqe_major_code, eqe) != 0)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0366 Not a valid fast-path completion "
				"event: majorcode=x%x, minorcode=x%x\n",
				bf_get_le32(lpfc_eqe_major_code, eqe),
				bf_get_le32(lpfc_eqe_minor_code, eqe));
		return;
	}

	cq = phba->sli4_hba.fcp_cq[fcp_cqidx];
	if (unlikely(!cq)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0367 Fast-path completion queue does not "
				"exist\n");
		return;
	}

	/* Get the reference to the corresponding CQ */
	cqid = bf_get_le32(lpfc_eqe_resource_id, eqe);
	if (unlikely(cqid != cq->queue_id)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0368 Miss-matched fast-path completion "
				"queue identifier: eqcqid=%d, fcpcqid=%d\n",
				cqid, cq->queue_id);
		return;
	}

	/* Process all the entries to the CQ */
	while ((cqe = lpfc_sli4_cq_get(cq))) {
		workposted |= lpfc_sli4_fp_handle_wcqe(phba, cq, cqe);
		if (!(++ecount % LPFC_GET_QE_REL_INT))
			lpfc_sli4_cq_release(cq, LPFC_QUEUE_NOARM);
	}

	/* Catch the no cq entry condition */
	if (unlikely(ecount == 0))
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0369 No entry from fast-path completion "
				"queue fcpcqid=%d\n", cq->queue_id);

	/* In any case, flash and re-arm the CQ */
	lpfc_sli4_cq_release(cq, LPFC_QUEUE_REARM);

	/* wake up worker thread if there are works to be done */
	if (workposted)
		lpfc_worker_wake_up(phba);
}

static void
lpfc_sli4_eq_flush(struct lpfc_hba *phba, struct lpfc_queue *eq)
{
	struct lpfc_eqe *eqe;

	/* walk all the EQ entries and drop on the floor */
	while ((eqe = lpfc_sli4_eq_get(eq)))
		;

	/* Clear and re-arm the EQ */
	lpfc_sli4_eq_release(eq, LPFC_QUEUE_REARM);
}

irqreturn_t
lpfc_sli4_sp_intr_handler(int irq, void *dev_id)
{
	struct lpfc_hba *phba;
	struct lpfc_queue *speq;
	struct lpfc_eqe *eqe;
	unsigned long iflag;
	int ecount = 0;

	/*
	 * Get the driver's phba structure from the dev_id
	 */
	phba = (struct lpfc_hba *)dev_id;

	if (unlikely(!phba))
		return IRQ_NONE;

	/* Get to the EQ struct associated with this vector */
	speq = phba->sli4_hba.sp_eq;

	/* Check device state for handling interrupt */
	if (unlikely(lpfc_intr_state_check(phba))) {
		/* Check again for link_state with lock held */
		spin_lock_irqsave(&phba->hbalock, iflag);
		if (phba->link_state < LPFC_LINK_DOWN)
			/* Flush, clear interrupt, and rearm the EQ */
			lpfc_sli4_eq_flush(phba, speq);
		spin_unlock_irqrestore(&phba->hbalock, iflag);
		return IRQ_NONE;
	}

	/*
	 * Process all the event on FCP slow-path EQ
	 */
	while ((eqe = lpfc_sli4_eq_get(speq))) {
		lpfc_sli4_sp_handle_eqe(phba, eqe);
		if (!(++ecount % LPFC_GET_QE_REL_INT))
			lpfc_sli4_eq_release(speq, LPFC_QUEUE_NOARM);
	}

	/* Always clear and re-arm the slow-path EQ */
	lpfc_sli4_eq_release(speq, LPFC_QUEUE_REARM);

	/* Catch the no cq entry condition */
	if (unlikely(ecount == 0)) {
		if (phba->intr_type == MSIX)
			/* MSI-X treated interrupt served as no EQ share INT */
			lpfc_printf_log(phba, KERN_WARNING, LOG_SLI,
					"0357 MSI-X interrupt with no EQE\n");
		else
			/* Non MSI-X treated on interrupt as EQ share INT */
			return IRQ_NONE;
	}

	return IRQ_HANDLED;
} /* lpfc_sli4_sp_intr_handler */

irqreturn_t
lpfc_sli4_fp_intr_handler(int irq, void *dev_id)
{
	struct lpfc_hba *phba;
	struct lpfc_fcp_eq_hdl *fcp_eq_hdl;
	struct lpfc_queue *fpeq;
	struct lpfc_eqe *eqe;
	unsigned long iflag;
	int ecount = 0;
	uint32_t fcp_eqidx;

	/* Get the driver's phba structure from the dev_id */
	fcp_eq_hdl = (struct lpfc_fcp_eq_hdl *)dev_id;
	phba = fcp_eq_hdl->phba;
	fcp_eqidx = fcp_eq_hdl->idx;

	if (unlikely(!phba))
		return IRQ_NONE;

	/* Get to the EQ struct associated with this vector */
	fpeq = phba->sli4_hba.fp_eq[fcp_eqidx];

	/* Check device state for handling interrupt */
	if (unlikely(lpfc_intr_state_check(phba))) {
		/* Check again for link_state with lock held */
		spin_lock_irqsave(&phba->hbalock, iflag);
		if (phba->link_state < LPFC_LINK_DOWN)
			/* Flush, clear interrupt, and rearm the EQ */
			lpfc_sli4_eq_flush(phba, fpeq);
		spin_unlock_irqrestore(&phba->hbalock, iflag);
		return IRQ_NONE;
	}

	/*
	 * Process all the event on FCP fast-path EQ
	 */
	while ((eqe = lpfc_sli4_eq_get(fpeq))) {
		lpfc_sli4_fp_handle_eqe(phba, eqe, fcp_eqidx);
		if (!(++ecount % LPFC_GET_QE_REL_INT))
			lpfc_sli4_eq_release(fpeq, LPFC_QUEUE_NOARM);
	}

	/* Always clear and re-arm the fast-path EQ */
	lpfc_sli4_eq_release(fpeq, LPFC_QUEUE_REARM);

	if (unlikely(ecount == 0)) {
		if (phba->intr_type == MSIX)
			/* MSI-X treated interrupt served as no EQ share INT */
			lpfc_printf_log(phba, KERN_WARNING, LOG_SLI,
					"0358 MSI-X interrupt with no EQE\n");
		else
			/* Non MSI-X treated on interrupt as EQ share INT */
			return IRQ_NONE;
	}

	return IRQ_HANDLED;
} /* lpfc_sli4_fp_intr_handler */

irqreturn_t
lpfc_sli4_intr_handler(int irq, void *dev_id)
{
	struct lpfc_hba  *phba;
	irqreturn_t sp_irq_rc, fp_irq_rc;
	bool fp_handled = false;
	uint32_t fcp_eqidx;

	/* Get the driver's phba structure from the dev_id */
	phba = (struct lpfc_hba *)dev_id;

	if (unlikely(!phba))
		return IRQ_NONE;

	/*
	 * Invokes slow-path host attention interrupt handling as appropriate.
	 */
	sp_irq_rc = lpfc_sli4_sp_intr_handler(irq, dev_id);

	/*
	 * Invoke fast-path host attention interrupt handling as appropriate.
	 */
	for (fcp_eqidx = 0; fcp_eqidx < phba->cfg_fcp_eq_count; fcp_eqidx++) {
		fp_irq_rc = lpfc_sli4_fp_intr_handler(irq,
					&phba->sli4_hba.fcp_eq_hdl[fcp_eqidx]);
		if (fp_irq_rc == IRQ_HANDLED)
			fp_handled |= true;
	}

	return (fp_handled == true) ? IRQ_HANDLED : sp_irq_rc;
} /* lpfc_sli4_intr_handler */

void
lpfc_sli4_queue_free(struct lpfc_queue *queue)
{
	struct lpfc_dmabuf *dmabuf;

	if (!queue)
		return;

	while (!list_empty(&queue->page_list)) {
		list_remove_head(&queue->page_list, dmabuf, struct lpfc_dmabuf,
				 list);
		dma_free_coherent(&queue->phba->pcidev->dev, SLI4_PAGE_SIZE,
				  dmabuf->virt, dmabuf->phys);
		kfree(dmabuf);
	}
	kfree(queue);
	return;
}

struct lpfc_queue *
lpfc_sli4_queue_alloc(struct lpfc_hba *phba, uint32_t entry_size,
		      uint32_t entry_count)
{
	struct lpfc_queue *queue;
	struct lpfc_dmabuf *dmabuf;
	int x, total_qe_count;
	void *dma_pointer;
	uint32_t hw_page_size = phba->sli4_hba.pc_sli4_params.if_page_sz;

	if (!phba->sli4_hba.pc_sli4_params.supported)
		hw_page_size = SLI4_PAGE_SIZE;

	queue = kzalloc(sizeof(struct lpfc_queue) +
			(sizeof(union sli4_qe) * entry_count), GFP_KERNEL);
	if (!queue)
		return NULL;
	queue->page_count = (ALIGN(entry_size * entry_count,
			hw_page_size))/hw_page_size;
	INIT_LIST_HEAD(&queue->list);
	INIT_LIST_HEAD(&queue->page_list);
	INIT_LIST_HEAD(&queue->child_list);
	for (x = 0, total_qe_count = 0; x < queue->page_count; x++) {
		dmabuf = kzalloc(sizeof(struct lpfc_dmabuf), GFP_KERNEL);
		if (!dmabuf)
			goto out_fail;
		dmabuf->virt = dma_alloc_coherent(&phba->pcidev->dev,
						  hw_page_size, &dmabuf->phys,
						  GFP_KERNEL);
		if (!dmabuf->virt) {
			kfree(dmabuf);
			goto out_fail;
		}
		memset(dmabuf->virt, 0, hw_page_size);
		dmabuf->buffer_tag = x;
		list_add_tail(&dmabuf->list, &queue->page_list);
		/* initialize queue's entry array */
		dma_pointer = dmabuf->virt;
		for (; total_qe_count < entry_count &&
		     dma_pointer < (hw_page_size + dmabuf->virt);
		     total_qe_count++, dma_pointer += entry_size) {
			queue->qe[total_qe_count].address = dma_pointer;
		}
	}
	queue->entry_size = entry_size;
	queue->entry_count = entry_count;
	queue->phba = phba;

	return queue;
out_fail:
	lpfc_sli4_queue_free(queue);
	return NULL;
}

uint32_t
lpfc_eq_create(struct lpfc_hba *phba, struct lpfc_queue *eq, uint16_t imax)
{
	struct lpfc_mbx_eq_create *eq_create;
	LPFC_MBOXQ_t *mbox;
	int rc, length, status = 0;
	struct lpfc_dmabuf *dmabuf;
	uint32_t shdr_status, shdr_add_status;
	union lpfc_sli4_cfg_shdr *shdr;
	uint16_t dmult;
	uint32_t hw_page_size = phba->sli4_hba.pc_sli4_params.if_page_sz;

	if (!phba->sli4_hba.pc_sli4_params.supported)
		hw_page_size = SLI4_PAGE_SIZE;

	mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;
	length = (sizeof(struct lpfc_mbx_eq_create) -
		  sizeof(struct lpfc_sli4_cfg_mhdr));
	lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_COMMON,
			 LPFC_MBOX_OPCODE_EQ_CREATE,
			 length, LPFC_SLI4_MBX_EMBED);
	eq_create = &mbox->u.mqe.un.eq_create;
	bf_set(lpfc_mbx_eq_create_num_pages, &eq_create->u.request,
	       eq->page_count);
	bf_set(lpfc_eq_context_size, &eq_create->u.request.context,
	       LPFC_EQE_SIZE);
	bf_set(lpfc_eq_context_valid, &eq_create->u.request.context, 1);
	/* Calculate delay multiper from maximum interrupt per second */
	dmult = LPFC_DMULT_CONST/imax - 1;
	bf_set(lpfc_eq_context_delay_multi, &eq_create->u.request.context,
	       dmult);
	switch (eq->entry_count) {
	default:
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0360 Unsupported EQ count. (%d)\n",
				eq->entry_count);
		if (eq->entry_count < 256)
			return -EINVAL;
		/* otherwise default to smallest count (drop through) */
	case 256:
		bf_set(lpfc_eq_context_count, &eq_create->u.request.context,
		       LPFC_EQ_CNT_256);
		break;
	case 512:
		bf_set(lpfc_eq_context_count, &eq_create->u.request.context,
		       LPFC_EQ_CNT_512);
		break;
	case 1024:
		bf_set(lpfc_eq_context_count, &eq_create->u.request.context,
		       LPFC_EQ_CNT_1024);
		break;
	case 2048:
		bf_set(lpfc_eq_context_count, &eq_create->u.request.context,
		       LPFC_EQ_CNT_2048);
		break;
	case 4096:
		bf_set(lpfc_eq_context_count, &eq_create->u.request.context,
		       LPFC_EQ_CNT_4096);
		break;
	}
	list_for_each_entry(dmabuf, &eq->page_list, list) {
		memset(dmabuf->virt, 0, hw_page_size);
		eq_create->u.request.page[dmabuf->buffer_tag].addr_lo =
					putPaddrLow(dmabuf->phys);
		eq_create->u.request.page[dmabuf->buffer_tag].addr_hi =
					putPaddrHigh(dmabuf->phys);
	}
	mbox->vport = phba->pport;
	mbox->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
	mbox->context1 = NULL;
	rc = lpfc_sli_issue_mbox(phba, mbox, MBX_POLL);
	shdr = (union lpfc_sli4_cfg_shdr *) &eq_create->header.cfg_shdr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (shdr_status || shdr_add_status || rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2500 EQ_CREATE mailbox failed with "
				"status x%x add_status x%x, mbx status x%x\n",
				shdr_status, shdr_add_status, rc);
		status = -ENXIO;
	}
	eq->type = LPFC_EQ;
	eq->subtype = LPFC_NONE;
	eq->queue_id = bf_get(lpfc_mbx_eq_create_q_id, &eq_create->u.response);
	if (eq->queue_id == 0xFFFF)
		status = -ENXIO;
	eq->host_index = 0;
	eq->hba_index = 0;

	mempool_free(mbox, phba->mbox_mem_pool);
	return status;
}

uint32_t
lpfc_cq_create(struct lpfc_hba *phba, struct lpfc_queue *cq,
	       struct lpfc_queue *eq, uint32_t type, uint32_t subtype)
{
	struct lpfc_mbx_cq_create *cq_create;
	struct lpfc_dmabuf *dmabuf;
	LPFC_MBOXQ_t *mbox;
	int rc, length, status = 0;
	uint32_t shdr_status, shdr_add_status;
	union lpfc_sli4_cfg_shdr *shdr;
	uint32_t hw_page_size = phba->sli4_hba.pc_sli4_params.if_page_sz;

	if (!phba->sli4_hba.pc_sli4_params.supported)
		hw_page_size = SLI4_PAGE_SIZE;


	mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;
	length = (sizeof(struct lpfc_mbx_cq_create) -
		  sizeof(struct lpfc_sli4_cfg_mhdr));
	lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_COMMON,
			 LPFC_MBOX_OPCODE_CQ_CREATE,
			 length, LPFC_SLI4_MBX_EMBED);
	cq_create = &mbox->u.mqe.un.cq_create;
	bf_set(lpfc_mbx_cq_create_num_pages, &cq_create->u.request,
		    cq->page_count);
	bf_set(lpfc_cq_context_event, &cq_create->u.request.context, 1);
	bf_set(lpfc_cq_context_valid, &cq_create->u.request.context, 1);
	bf_set(lpfc_cq_eq_id, &cq_create->u.request.context, eq->queue_id);
	switch (cq->entry_count) {
	default:
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0361 Unsupported CQ count. (%d)\n",
				cq->entry_count);
		if (cq->entry_count < 256)
			return -EINVAL;
		/* otherwise default to smallest count (drop through) */
	case 256:
		bf_set(lpfc_cq_context_count, &cq_create->u.request.context,
		       LPFC_CQ_CNT_256);
		break;
	case 512:
		bf_set(lpfc_cq_context_count, &cq_create->u.request.context,
		       LPFC_CQ_CNT_512);
		break;
	case 1024:
		bf_set(lpfc_cq_context_count, &cq_create->u.request.context,
		       LPFC_CQ_CNT_1024);
		break;
	}
	list_for_each_entry(dmabuf, &cq->page_list, list) {
		memset(dmabuf->virt, 0, hw_page_size);
		cq_create->u.request.page[dmabuf->buffer_tag].addr_lo =
					putPaddrLow(dmabuf->phys);
		cq_create->u.request.page[dmabuf->buffer_tag].addr_hi =
					putPaddrHigh(dmabuf->phys);
	}
	rc = lpfc_sli_issue_mbox(phba, mbox, MBX_POLL);

	/* The IOCTL status is embedded in the mailbox subheader. */
	shdr = (union lpfc_sli4_cfg_shdr *) &cq_create->header.cfg_shdr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (shdr_status || shdr_add_status || rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2501 CQ_CREATE mailbox failed with "
				"status x%x add_status x%x, mbx status x%x\n",
				shdr_status, shdr_add_status, rc);
		status = -ENXIO;
		goto out;
	}
	cq->queue_id = bf_get(lpfc_mbx_cq_create_q_id, &cq_create->u.response);
	if (cq->queue_id == 0xFFFF) {
		status = -ENXIO;
		goto out;
	}
	/* link the cq onto the parent eq child list */
	list_add_tail(&cq->list, &eq->child_list);
	/* Set up completion queue's type and subtype */
	cq->type = type;
	cq->subtype = subtype;
	cq->queue_id = bf_get(lpfc_mbx_cq_create_q_id, &cq_create->u.response);
	cq->host_index = 0;
	cq->hba_index = 0;

out:
	mempool_free(mbox, phba->mbox_mem_pool);
	return status;
}

static void
lpfc_mq_create_fb_init(struct lpfc_hba *phba, struct lpfc_queue *mq,
		       LPFC_MBOXQ_t *mbox, struct lpfc_queue *cq)
{
	struct lpfc_mbx_mq_create *mq_create;
	struct lpfc_dmabuf *dmabuf;
	int length;

	length = (sizeof(struct lpfc_mbx_mq_create) -
		  sizeof(struct lpfc_sli4_cfg_mhdr));
	lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_COMMON,
			 LPFC_MBOX_OPCODE_MQ_CREATE,
			 length, LPFC_SLI4_MBX_EMBED);
	mq_create = &mbox->u.mqe.un.mq_create;
	bf_set(lpfc_mbx_mq_create_num_pages, &mq_create->u.request,
	       mq->page_count);
	bf_set(lpfc_mq_context_cq_id, &mq_create->u.request.context,
	       cq->queue_id);
	bf_set(lpfc_mq_context_valid, &mq_create->u.request.context, 1);
	switch (mq->entry_count) {
	case 16:
		bf_set(lpfc_mq_context_count, &mq_create->u.request.context,
		       LPFC_MQ_CNT_16);
		break;
	case 32:
		bf_set(lpfc_mq_context_count, &mq_create->u.request.context,
		       LPFC_MQ_CNT_32);
		break;
	case 64:
		bf_set(lpfc_mq_context_count, &mq_create->u.request.context,
		       LPFC_MQ_CNT_64);
		break;
	case 128:
		bf_set(lpfc_mq_context_count, &mq_create->u.request.context,
		       LPFC_MQ_CNT_128);
		break;
	}
	list_for_each_entry(dmabuf, &mq->page_list, list) {
		mq_create->u.request.page[dmabuf->buffer_tag].addr_lo =
			putPaddrLow(dmabuf->phys);
		mq_create->u.request.page[dmabuf->buffer_tag].addr_hi =
			putPaddrHigh(dmabuf->phys);
	}
}

int32_t
lpfc_mq_create(struct lpfc_hba *phba, struct lpfc_queue *mq,
	       struct lpfc_queue *cq, uint32_t subtype)
{
	struct lpfc_mbx_mq_create *mq_create;
	struct lpfc_mbx_mq_create_ext *mq_create_ext;
	struct lpfc_dmabuf *dmabuf;
	LPFC_MBOXQ_t *mbox;
	int rc, length, status = 0;
	uint32_t shdr_status, shdr_add_status;
	union lpfc_sli4_cfg_shdr *shdr;
	uint32_t hw_page_size = phba->sli4_hba.pc_sli4_params.if_page_sz;

	if (!phba->sli4_hba.pc_sli4_params.supported)
		hw_page_size = SLI4_PAGE_SIZE;

	mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;
	length = (sizeof(struct lpfc_mbx_mq_create_ext) -
		  sizeof(struct lpfc_sli4_cfg_mhdr));
	lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_COMMON,
			 LPFC_MBOX_OPCODE_MQ_CREATE_EXT,
			 length, LPFC_SLI4_MBX_EMBED);

	mq_create_ext = &mbox->u.mqe.un.mq_create_ext;
	bf_set(lpfc_mbx_mq_create_ext_num_pages, &mq_create_ext->u.request,
		    mq->page_count);
	bf_set(lpfc_mbx_mq_create_ext_async_evt_link, &mq_create_ext->u.request,
	       1);
	bf_set(lpfc_mbx_mq_create_ext_async_evt_fcfste,
	       &mq_create_ext->u.request, 1);
	bf_set(lpfc_mbx_mq_create_ext_async_evt_group5,
	       &mq_create_ext->u.request, 1);
	bf_set(lpfc_mq_context_cq_id, &mq_create_ext->u.request.context,
	       cq->queue_id);
	bf_set(lpfc_mq_context_valid, &mq_create_ext->u.request.context, 1);
	switch (mq->entry_count) {
	default:
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0362 Unsupported MQ count. (%d)\n",
				mq->entry_count);
		if (mq->entry_count < 16)
			return -EINVAL;
		/* otherwise default to smallest count (drop through) */
	case 16:
		bf_set(lpfc_mq_context_count, &mq_create_ext->u.request.context,
		       LPFC_MQ_CNT_16);
		break;
	case 32:
		bf_set(lpfc_mq_context_count, &mq_create_ext->u.request.context,
		       LPFC_MQ_CNT_32);
		break;
	case 64:
		bf_set(lpfc_mq_context_count, &mq_create_ext->u.request.context,
		       LPFC_MQ_CNT_64);
		break;
	case 128:
		bf_set(lpfc_mq_context_count, &mq_create_ext->u.request.context,
		       LPFC_MQ_CNT_128);
		break;
	}
	list_for_each_entry(dmabuf, &mq->page_list, list) {
		memset(dmabuf->virt, 0, hw_page_size);
		mq_create_ext->u.request.page[dmabuf->buffer_tag].addr_lo =
					putPaddrLow(dmabuf->phys);
		mq_create_ext->u.request.page[dmabuf->buffer_tag].addr_hi =
					putPaddrHigh(dmabuf->phys);
	}
	rc = lpfc_sli_issue_mbox(phba, mbox, MBX_POLL);
	shdr = (union lpfc_sli4_cfg_shdr *) &mq_create_ext->header.cfg_shdr;
	mq->queue_id = bf_get(lpfc_mbx_mq_create_q_id,
			      &mq_create_ext->u.response);
	if (rc != MBX_SUCCESS) {
		lpfc_printf_log(phba, KERN_INFO, LOG_INIT,
				"2795 MQ_CREATE_EXT failed with "
				"status x%x. Failback to MQ_CREATE.\n",
				rc);
		lpfc_mq_create_fb_init(phba, mq, mbox, cq);
		mq_create = &mbox->u.mqe.un.mq_create;
		rc = lpfc_sli_issue_mbox(phba, mbox, MBX_POLL);
		shdr = (union lpfc_sli4_cfg_shdr *) &mq_create->header.cfg_shdr;
		mq->queue_id = bf_get(lpfc_mbx_mq_create_q_id,
				      &mq_create->u.response);
	}

	/* The IOCTL status is embedded in the mailbox subheader. */
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (shdr_status || shdr_add_status || rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2502 MQ_CREATE mailbox failed with "
				"status x%x add_status x%x, mbx status x%x\n",
				shdr_status, shdr_add_status, rc);
		status = -ENXIO;
		goto out;
	}
	if (mq->queue_id == 0xFFFF) {
		status = -ENXIO;
		goto out;
	}
	mq->type = LPFC_MQ;
	mq->subtype = subtype;
	mq->host_index = 0;
	mq->hba_index = 0;

	/* link the mq onto the parent cq child list */
	list_add_tail(&mq->list, &cq->child_list);
out:
	mempool_free(mbox, phba->mbox_mem_pool);
	return status;
}

uint32_t
lpfc_wq_create(struct lpfc_hba *phba, struct lpfc_queue *wq,
	       struct lpfc_queue *cq, uint32_t subtype)
{
	struct lpfc_mbx_wq_create *wq_create;
	struct lpfc_dmabuf *dmabuf;
	LPFC_MBOXQ_t *mbox;
	int rc, length, status = 0;
	uint32_t shdr_status, shdr_add_status;
	union lpfc_sli4_cfg_shdr *shdr;
	uint32_t hw_page_size = phba->sli4_hba.pc_sli4_params.if_page_sz;

	if (!phba->sli4_hba.pc_sli4_params.supported)
		hw_page_size = SLI4_PAGE_SIZE;

	mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;
	length = (sizeof(struct lpfc_mbx_wq_create) -
		  sizeof(struct lpfc_sli4_cfg_mhdr));
	lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_FCOE,
			 LPFC_MBOX_OPCODE_FCOE_WQ_CREATE,
			 length, LPFC_SLI4_MBX_EMBED);
	wq_create = &mbox->u.mqe.un.wq_create;
	bf_set(lpfc_mbx_wq_create_num_pages, &wq_create->u.request,
		    wq->page_count);
	bf_set(lpfc_mbx_wq_create_cq_id, &wq_create->u.request,
		    cq->queue_id);
	list_for_each_entry(dmabuf, &wq->page_list, list) {
		memset(dmabuf->virt, 0, hw_page_size);
		wq_create->u.request.page[dmabuf->buffer_tag].addr_lo =
					putPaddrLow(dmabuf->phys);
		wq_create->u.request.page[dmabuf->buffer_tag].addr_hi =
					putPaddrHigh(dmabuf->phys);
	}
	rc = lpfc_sli_issue_mbox(phba, mbox, MBX_POLL);
	/* The IOCTL status is embedded in the mailbox subheader. */
	shdr = (union lpfc_sli4_cfg_shdr *) &wq_create->header.cfg_shdr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (shdr_status || shdr_add_status || rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2503 WQ_CREATE mailbox failed with "
				"status x%x add_status x%x, mbx status x%x\n",
				shdr_status, shdr_add_status, rc);
		status = -ENXIO;
		goto out;
	}
	wq->queue_id = bf_get(lpfc_mbx_wq_create_q_id, &wq_create->u.response);
	if (wq->queue_id == 0xFFFF) {
		status = -ENXIO;
		goto out;
	}
	wq->type = LPFC_WQ;
	wq->subtype = subtype;
	wq->host_index = 0;
	wq->hba_index = 0;

	/* link the wq onto the parent cq child list */
	list_add_tail(&wq->list, &cq->child_list);
out:
	mempool_free(mbox, phba->mbox_mem_pool);
	return status;
}

uint32_t
lpfc_rq_create(struct lpfc_hba *phba, struct lpfc_queue *hrq,
	       struct lpfc_queue *drq, struct lpfc_queue *cq, uint32_t subtype)
{
	struct lpfc_mbx_rq_create *rq_create;
	struct lpfc_dmabuf *dmabuf;
	LPFC_MBOXQ_t *mbox;
	int rc, length, status = 0;
	uint32_t shdr_status, shdr_add_status;
	union lpfc_sli4_cfg_shdr *shdr;
	uint32_t hw_page_size = phba->sli4_hba.pc_sli4_params.if_page_sz;

	if (!phba->sli4_hba.pc_sli4_params.supported)
		hw_page_size = SLI4_PAGE_SIZE;

	if (hrq->entry_count != drq->entry_count)
		return -EINVAL;
	mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;
	length = (sizeof(struct lpfc_mbx_rq_create) -
		  sizeof(struct lpfc_sli4_cfg_mhdr));
	lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_FCOE,
			 LPFC_MBOX_OPCODE_FCOE_RQ_CREATE,
			 length, LPFC_SLI4_MBX_EMBED);
	rq_create = &mbox->u.mqe.un.rq_create;
	switch (hrq->entry_count) {
	default:
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"2535 Unsupported RQ count. (%d)\n",
				hrq->entry_count);
		if (hrq->entry_count < 512)
			return -EINVAL;
		/* otherwise default to smallest count (drop through) */
	case 512:
		bf_set(lpfc_rq_context_rq_size, &rq_create->u.request.context,
		       LPFC_RQ_RING_SIZE_512);
		break;
	case 1024:
		bf_set(lpfc_rq_context_rq_size, &rq_create->u.request.context,
		       LPFC_RQ_RING_SIZE_1024);
		break;
	case 2048:
		bf_set(lpfc_rq_context_rq_size, &rq_create->u.request.context,
		       LPFC_RQ_RING_SIZE_2048);
		break;
	case 4096:
		bf_set(lpfc_rq_context_rq_size, &rq_create->u.request.context,
		       LPFC_RQ_RING_SIZE_4096);
		break;
	}
	bf_set(lpfc_rq_context_cq_id, &rq_create->u.request.context,
	       cq->queue_id);
	bf_set(lpfc_mbx_rq_create_num_pages, &rq_create->u.request,
	       hrq->page_count);
	bf_set(lpfc_rq_context_buf_size, &rq_create->u.request.context,
	       LPFC_HDR_BUF_SIZE);
	list_for_each_entry(dmabuf, &hrq->page_list, list) {
		memset(dmabuf->virt, 0, hw_page_size);
		rq_create->u.request.page[dmabuf->buffer_tag].addr_lo =
					putPaddrLow(dmabuf->phys);
		rq_create->u.request.page[dmabuf->buffer_tag].addr_hi =
					putPaddrHigh(dmabuf->phys);
	}
	rc = lpfc_sli_issue_mbox(phba, mbox, MBX_POLL);
	/* The IOCTL status is embedded in the mailbox subheader. */
	shdr = (union lpfc_sli4_cfg_shdr *) &rq_create->header.cfg_shdr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (shdr_status || shdr_add_status || rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2504 RQ_CREATE mailbox failed with "
				"status x%x add_status x%x, mbx status x%x\n",
				shdr_status, shdr_add_status, rc);
		status = -ENXIO;
		goto out;
	}
	hrq->queue_id = bf_get(lpfc_mbx_rq_create_q_id, &rq_create->u.response);
	if (hrq->queue_id == 0xFFFF) {
		status = -ENXIO;
		goto out;
	}
	hrq->type = LPFC_HRQ;
	hrq->subtype = subtype;
	hrq->host_index = 0;
	hrq->hba_index = 0;

	/* now create the data queue */
	lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_FCOE,
			 LPFC_MBOX_OPCODE_FCOE_RQ_CREATE,
			 length, LPFC_SLI4_MBX_EMBED);
	switch (drq->entry_count) {
	default:
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"2536 Unsupported RQ count. (%d)\n",
				drq->entry_count);
		if (drq->entry_count < 512)
			return -EINVAL;
		/* otherwise default to smallest count (drop through) */
	case 512:
		bf_set(lpfc_rq_context_rq_size, &rq_create->u.request.context,
		       LPFC_RQ_RING_SIZE_512);
		break;
	case 1024:
		bf_set(lpfc_rq_context_rq_size, &rq_create->u.request.context,
		       LPFC_RQ_RING_SIZE_1024);
		break;
	case 2048:
		bf_set(lpfc_rq_context_rq_size, &rq_create->u.request.context,
		       LPFC_RQ_RING_SIZE_2048);
		break;
	case 4096:
		bf_set(lpfc_rq_context_rq_size, &rq_create->u.request.context,
		       LPFC_RQ_RING_SIZE_4096);
		break;
	}
	bf_set(lpfc_rq_context_cq_id, &rq_create->u.request.context,
	       cq->queue_id);
	bf_set(lpfc_mbx_rq_create_num_pages, &rq_create->u.request,
	       drq->page_count);
	bf_set(lpfc_rq_context_buf_size, &rq_create->u.request.context,
	       LPFC_DATA_BUF_SIZE);
	list_for_each_entry(dmabuf, &drq->page_list, list) {
		rq_create->u.request.page[dmabuf->buffer_tag].addr_lo =
					putPaddrLow(dmabuf->phys);
		rq_create->u.request.page[dmabuf->buffer_tag].addr_hi =
					putPaddrHigh(dmabuf->phys);
	}
	rc = lpfc_sli_issue_mbox(phba, mbox, MBX_POLL);
	/* The IOCTL status is embedded in the mailbox subheader. */
	shdr = (union lpfc_sli4_cfg_shdr *) &rq_create->header.cfg_shdr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (shdr_status || shdr_add_status || rc) {
		status = -ENXIO;
		goto out;
	}
	drq->queue_id = bf_get(lpfc_mbx_rq_create_q_id, &rq_create->u.response);
	if (drq->queue_id == 0xFFFF) {
		status = -ENXIO;
		goto out;
	}
	drq->type = LPFC_DRQ;
	drq->subtype = subtype;
	drq->host_index = 0;
	drq->hba_index = 0;

	/* link the header and data RQs onto the parent cq child list */
	list_add_tail(&hrq->list, &cq->child_list);
	list_add_tail(&drq->list, &cq->child_list);

out:
	mempool_free(mbox, phba->mbox_mem_pool);
	return status;
}

uint32_t
lpfc_eq_destroy(struct lpfc_hba *phba, struct lpfc_queue *eq)
{
	LPFC_MBOXQ_t *mbox;
	int rc, length, status = 0;
	uint32_t shdr_status, shdr_add_status;
	union lpfc_sli4_cfg_shdr *shdr;

	if (!eq)
		return -ENODEV;
	mbox = mempool_alloc(eq->phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;
	length = (sizeof(struct lpfc_mbx_eq_destroy) -
		  sizeof(struct lpfc_sli4_cfg_mhdr));
	lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_COMMON,
			 LPFC_MBOX_OPCODE_EQ_DESTROY,
			 length, LPFC_SLI4_MBX_EMBED);
	bf_set(lpfc_mbx_eq_destroy_q_id, &mbox->u.mqe.un.eq_destroy.u.request,
	       eq->queue_id);
	mbox->vport = eq->phba->pport;
	mbox->mbox_cmpl = lpfc_sli_def_mbox_cmpl;

	rc = lpfc_sli_issue_mbox(eq->phba, mbox, MBX_POLL);
	/* The IOCTL status is embedded in the mailbox subheader. */
	shdr = (union lpfc_sli4_cfg_shdr *)
		&mbox->u.mqe.un.eq_destroy.header.cfg_shdr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (shdr_status || shdr_add_status || rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2505 EQ_DESTROY mailbox failed with "
				"status x%x add_status x%x, mbx status x%x\n",
				shdr_status, shdr_add_status, rc);
		status = -ENXIO;
	}

	/* Remove eq from any list */
	list_del_init(&eq->list);
	mempool_free(mbox, eq->phba->mbox_mem_pool);
	return status;
}

uint32_t
lpfc_cq_destroy(struct lpfc_hba *phba, struct lpfc_queue *cq)
{
	LPFC_MBOXQ_t *mbox;
	int rc, length, status = 0;
	uint32_t shdr_status, shdr_add_status;
	union lpfc_sli4_cfg_shdr *shdr;

	if (!cq)
		return -ENODEV;
	mbox = mempool_alloc(cq->phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;
	length = (sizeof(struct lpfc_mbx_cq_destroy) -
		  sizeof(struct lpfc_sli4_cfg_mhdr));
	lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_COMMON,
			 LPFC_MBOX_OPCODE_CQ_DESTROY,
			 length, LPFC_SLI4_MBX_EMBED);
	bf_set(lpfc_mbx_cq_destroy_q_id, &mbox->u.mqe.un.cq_destroy.u.request,
	       cq->queue_id);
	mbox->vport = cq->phba->pport;
	mbox->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
	rc = lpfc_sli_issue_mbox(cq->phba, mbox, MBX_POLL);
	/* The IOCTL status is embedded in the mailbox subheader. */
	shdr = (union lpfc_sli4_cfg_shdr *)
		&mbox->u.mqe.un.wq_create.header.cfg_shdr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (shdr_status || shdr_add_status || rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2506 CQ_DESTROY mailbox failed with "
				"status x%x add_status x%x, mbx status x%x\n",
				shdr_status, shdr_add_status, rc);
		status = -ENXIO;
	}
	/* Remove cq from any list */
	list_del_init(&cq->list);
	mempool_free(mbox, cq->phba->mbox_mem_pool);
	return status;
}

uint32_t
lpfc_mq_destroy(struct lpfc_hba *phba, struct lpfc_queue *mq)
{
	LPFC_MBOXQ_t *mbox;
	int rc, length, status = 0;
	uint32_t shdr_status, shdr_add_status;
	union lpfc_sli4_cfg_shdr *shdr;

	if (!mq)
		return -ENODEV;
	mbox = mempool_alloc(mq->phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;
	length = (sizeof(struct lpfc_mbx_mq_destroy) -
		  sizeof(struct lpfc_sli4_cfg_mhdr));
	lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_COMMON,
			 LPFC_MBOX_OPCODE_MQ_DESTROY,
			 length, LPFC_SLI4_MBX_EMBED);
	bf_set(lpfc_mbx_mq_destroy_q_id, &mbox->u.mqe.un.mq_destroy.u.request,
	       mq->queue_id);
	mbox->vport = mq->phba->pport;
	mbox->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
	rc = lpfc_sli_issue_mbox(mq->phba, mbox, MBX_POLL);
	/* The IOCTL status is embedded in the mailbox subheader. */
	shdr = (union lpfc_sli4_cfg_shdr *)
		&mbox->u.mqe.un.mq_destroy.header.cfg_shdr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (shdr_status || shdr_add_status || rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2507 MQ_DESTROY mailbox failed with "
				"status x%x add_status x%x, mbx status x%x\n",
				shdr_status, shdr_add_status, rc);
		status = -ENXIO;
	}
	/* Remove mq from any list */
	list_del_init(&mq->list);
	mempool_free(mbox, mq->phba->mbox_mem_pool);
	return status;
}

uint32_t
lpfc_wq_destroy(struct lpfc_hba *phba, struct lpfc_queue *wq)
{
	LPFC_MBOXQ_t *mbox;
	int rc, length, status = 0;
	uint32_t shdr_status, shdr_add_status;
	union lpfc_sli4_cfg_shdr *shdr;

	if (!wq)
		return -ENODEV;
	mbox = mempool_alloc(wq->phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;
	length = (sizeof(struct lpfc_mbx_wq_destroy) -
		  sizeof(struct lpfc_sli4_cfg_mhdr));
	lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_FCOE,
			 LPFC_MBOX_OPCODE_FCOE_WQ_DESTROY,
			 length, LPFC_SLI4_MBX_EMBED);
	bf_set(lpfc_mbx_wq_destroy_q_id, &mbox->u.mqe.un.wq_destroy.u.request,
	       wq->queue_id);
	mbox->vport = wq->phba->pport;
	mbox->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
	rc = lpfc_sli_issue_mbox(wq->phba, mbox, MBX_POLL);
	shdr = (union lpfc_sli4_cfg_shdr *)
		&mbox->u.mqe.un.wq_destroy.header.cfg_shdr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (shdr_status || shdr_add_status || rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2508 WQ_DESTROY mailbox failed with "
				"status x%x add_status x%x, mbx status x%x\n",
				shdr_status, shdr_add_status, rc);
		status = -ENXIO;
	}
	/* Remove wq from any list */
	list_del_init(&wq->list);
	mempool_free(mbox, wq->phba->mbox_mem_pool);
	return status;
}

uint32_t
lpfc_rq_destroy(struct lpfc_hba *phba, struct lpfc_queue *hrq,
		struct lpfc_queue *drq)
{
	LPFC_MBOXQ_t *mbox;
	int rc, length, status = 0;
	uint32_t shdr_status, shdr_add_status;
	union lpfc_sli4_cfg_shdr *shdr;

	if (!hrq || !drq)
		return -ENODEV;
	mbox = mempool_alloc(hrq->phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;
	length = (sizeof(struct lpfc_mbx_rq_destroy) -
		  sizeof(struct mbox_header));
	lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_FCOE,
			 LPFC_MBOX_OPCODE_FCOE_RQ_DESTROY,
			 length, LPFC_SLI4_MBX_EMBED);
	bf_set(lpfc_mbx_rq_destroy_q_id, &mbox->u.mqe.un.rq_destroy.u.request,
	       hrq->queue_id);
	mbox->vport = hrq->phba->pport;
	mbox->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
	rc = lpfc_sli_issue_mbox(hrq->phba, mbox, MBX_POLL);
	/* The IOCTL status is embedded in the mailbox subheader. */
	shdr = (union lpfc_sli4_cfg_shdr *)
		&mbox->u.mqe.un.rq_destroy.header.cfg_shdr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (shdr_status || shdr_add_status || rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2509 RQ_DESTROY mailbox failed with "
				"status x%x add_status x%x, mbx status x%x\n",
				shdr_status, shdr_add_status, rc);
		if (rc != MBX_TIMEOUT)
			mempool_free(mbox, hrq->phba->mbox_mem_pool);
		return -ENXIO;
	}
	bf_set(lpfc_mbx_rq_destroy_q_id, &mbox->u.mqe.un.rq_destroy.u.request,
	       drq->queue_id);
	rc = lpfc_sli_issue_mbox(drq->phba, mbox, MBX_POLL);
	shdr = (union lpfc_sli4_cfg_shdr *)
		&mbox->u.mqe.un.rq_destroy.header.cfg_shdr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (shdr_status || shdr_add_status || rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2510 RQ_DESTROY mailbox failed with "
				"status x%x add_status x%x, mbx status x%x\n",
				shdr_status, shdr_add_status, rc);
		status = -ENXIO;
	}
	list_del_init(&hrq->list);
	list_del_init(&drq->list);
	mempool_free(mbox, hrq->phba->mbox_mem_pool);
	return status;
}

int
lpfc_sli4_post_sgl(struct lpfc_hba *phba,
		dma_addr_t pdma_phys_addr0,
		dma_addr_t pdma_phys_addr1,
		uint16_t xritag)
{
	struct lpfc_mbx_post_sgl_pages *post_sgl_pages;
	LPFC_MBOXQ_t *mbox;
	int rc;
	uint32_t shdr_status, shdr_add_status;
	union lpfc_sli4_cfg_shdr *shdr;

	if (xritag == NO_XRI) {
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"0364 Invalid param:\n");
		return -EINVAL;
	}

	mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;

	lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_FCOE,
			LPFC_MBOX_OPCODE_FCOE_POST_SGL_PAGES,
			sizeof(struct lpfc_mbx_post_sgl_pages) -
			sizeof(struct mbox_header), LPFC_SLI4_MBX_EMBED);

	post_sgl_pages = (struct lpfc_mbx_post_sgl_pages *)
				&mbox->u.mqe.un.post_sgl_pages;
	bf_set(lpfc_post_sgl_pages_xri, post_sgl_pages, xritag);
	bf_set(lpfc_post_sgl_pages_xricnt, post_sgl_pages, 1);

	post_sgl_pages->sgl_pg_pairs[0].sgl_pg0_addr_lo	=
				cpu_to_le32(putPaddrLow(pdma_phys_addr0));
	post_sgl_pages->sgl_pg_pairs[0].sgl_pg0_addr_hi =
				cpu_to_le32(putPaddrHigh(pdma_phys_addr0));

	post_sgl_pages->sgl_pg_pairs[0].sgl_pg1_addr_lo	=
				cpu_to_le32(putPaddrLow(pdma_phys_addr1));
	post_sgl_pages->sgl_pg_pairs[0].sgl_pg1_addr_hi =
				cpu_to_le32(putPaddrHigh(pdma_phys_addr1));
	if (!phba->sli4_hba.intr_enable)
		rc = lpfc_sli_issue_mbox(phba, mbox, MBX_POLL);
	else
		rc = lpfc_sli_issue_mbox_wait(phba, mbox, LPFC_MBOX_TMO);
	/* The IOCTL status is embedded in the mailbox subheader. */
	shdr = (union lpfc_sli4_cfg_shdr *) &post_sgl_pages->header.cfg_shdr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (rc != MBX_TIMEOUT)
		mempool_free(mbox, phba->mbox_mem_pool);
	if (shdr_status || shdr_add_status || rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2511 POST_SGL mailbox failed with "
				"status x%x add_status x%x, mbx status x%x\n",
				shdr_status, shdr_add_status, rc);
		rc = -ENXIO;
	}
	return 0;
}
int
lpfc_sli4_remove_all_sgl_pages(struct lpfc_hba *phba)
{
	LPFC_MBOXQ_t *mbox;
	int rc;
	uint32_t shdr_status, shdr_add_status;
	union lpfc_sli4_cfg_shdr *shdr;

	mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;

	lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_FCOE,
			LPFC_MBOX_OPCODE_FCOE_REMOVE_SGL_PAGES, 0,
			LPFC_SLI4_MBX_EMBED);
	if (!phba->sli4_hba.intr_enable)
		rc = lpfc_sli_issue_mbox(phba, mbox, MBX_POLL);
	else
		rc = lpfc_sli_issue_mbox_wait(phba, mbox, LPFC_MBOX_TMO);
	/* The IOCTL status is embedded in the mailbox subheader. */
	shdr = (union lpfc_sli4_cfg_shdr *)
		&mbox->u.mqe.un.sli4_config.header.cfg_shdr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (rc != MBX_TIMEOUT)
		mempool_free(mbox, phba->mbox_mem_pool);
	if (shdr_status || shdr_add_status || rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2512 REMOVE_ALL_SGL_PAGES mailbox failed with "
				"status x%x add_status x%x, mbx status x%x\n",
				shdr_status, shdr_add_status, rc);
		rc = -ENXIO;
	}
	return rc;
}

uint16_t
lpfc_sli4_next_xritag(struct lpfc_hba *phba)
{
	uint16_t xritag;

	spin_lock_irq(&phba->hbalock);
	xritag = phba->sli4_hba.next_xri;
	if ((xritag != (uint16_t) -1) && xritag <
		(phba->sli4_hba.max_cfg_param.max_xri
			+ phba->sli4_hba.max_cfg_param.xri_base)) {
		phba->sli4_hba.next_xri++;
		phba->sli4_hba.max_cfg_param.xri_used++;
		spin_unlock_irq(&phba->hbalock);
		return xritag;
	}
	spin_unlock_irq(&phba->hbalock);
	lpfc_printf_log(phba, KERN_WARNING, LOG_SLI,
			"2004 Failed to allocate XRI.last XRITAG is %d"
			" Max XRI is %d, Used XRI is %d\n",
			phba->sli4_hba.next_xri,
			phba->sli4_hba.max_cfg_param.max_xri,
			phba->sli4_hba.max_cfg_param.xri_used);
	return -1;
}

int
lpfc_sli4_post_sgl_list(struct lpfc_hba *phba)
{
	struct lpfc_sglq *sglq_entry;
	struct lpfc_mbx_post_uembed_sgl_page1 *sgl;
	struct sgl_page_pairs *sgl_pg_pairs;
	void *viraddr;
	LPFC_MBOXQ_t *mbox;
	uint32_t reqlen, alloclen, pg_pairs;
	uint32_t mbox_tmo;
	uint16_t xritag_start = 0;
	int els_xri_cnt, rc = 0;
	uint32_t shdr_status, shdr_add_status;
	union lpfc_sli4_cfg_shdr *shdr;

	/* The number of sgls to be posted */
	els_xri_cnt = lpfc_sli4_get_els_iocb_cnt(phba);

	reqlen = els_xri_cnt * sizeof(struct sgl_page_pairs) +
		 sizeof(union lpfc_sli4_cfg_shdr) + sizeof(uint32_t);
	if (reqlen > SLI4_PAGE_SIZE) {
		lpfc_printf_log(phba, KERN_WARNING, LOG_INIT,
				"2559 Block sgl registration required DMA "
				"size (%d) great than a page\n", reqlen);
		return -ENOMEM;
	}
	mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2560 Failed to allocate mbox cmd memory\n");
		return -ENOMEM;
	}

	/* Allocate DMA memory and set up the non-embedded mailbox command */
	alloclen = lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_FCOE,
			 LPFC_MBOX_OPCODE_FCOE_POST_SGL_PAGES, reqlen,
			 LPFC_SLI4_MBX_NEMBED);

	if (alloclen < reqlen) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"0285 Allocated DMA memory size (%d) is "
				"less than the requested DMA memory "
				"size (%d)\n", alloclen, reqlen);
		lpfc_sli4_mbox_cmd_free(phba, mbox);
		return -ENOMEM;
	}
	/* Get the first SGE entry from the non-embedded DMA memory */
	viraddr = mbox->sge_array->addr[0];

	/* Set up the SGL pages in the non-embedded DMA pages */
	sgl = (struct lpfc_mbx_post_uembed_sgl_page1 *)viraddr;
	sgl_pg_pairs = &sgl->sgl_pg_pairs;

	for (pg_pairs = 0; pg_pairs < els_xri_cnt; pg_pairs++) {
		sglq_entry = phba->sli4_hba.lpfc_els_sgl_array[pg_pairs];
		/* Set up the sge entry */
		sgl_pg_pairs->sgl_pg0_addr_lo =
				cpu_to_le32(putPaddrLow(sglq_entry->phys));
		sgl_pg_pairs->sgl_pg0_addr_hi =
				cpu_to_le32(putPaddrHigh(sglq_entry->phys));
		sgl_pg_pairs->sgl_pg1_addr_lo =
				cpu_to_le32(putPaddrLow(0));
		sgl_pg_pairs->sgl_pg1_addr_hi =
				cpu_to_le32(putPaddrHigh(0));
		/* Keep the first xritag on the list */
		if (pg_pairs == 0)
			xritag_start = sglq_entry->sli4_xritag;
		sgl_pg_pairs++;
	}
	bf_set(lpfc_post_sgl_pages_xri, sgl, xritag_start);
	bf_set(lpfc_post_sgl_pages_xricnt, sgl, els_xri_cnt);
	/* Perform endian conversion if necessary */
	sgl->word0 = cpu_to_le32(sgl->word0);

	if (!phba->sli4_hba.intr_enable)
		rc = lpfc_sli_issue_mbox(phba, mbox, MBX_POLL);
	else {
		mbox_tmo = lpfc_mbox_tmo_val(phba, MBX_SLI4_CONFIG);
		rc = lpfc_sli_issue_mbox_wait(phba, mbox, mbox_tmo);
	}
	shdr = (union lpfc_sli4_cfg_shdr *) &sgl->cfg_shdr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (rc != MBX_TIMEOUT)
		lpfc_sli4_mbox_cmd_free(phba, mbox);
	if (shdr_status || shdr_add_status || rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"2513 POST_SGL_BLOCK mailbox command failed "
				"status x%x add_status x%x mbx status x%x\n",
				shdr_status, shdr_add_status, rc);
		rc = -ENXIO;
	}
	return rc;
}

int
lpfc_sli4_post_scsi_sgl_block(struct lpfc_hba *phba, struct list_head *sblist,
			      int cnt)
{
	struct lpfc_scsi_buf *psb;
	struct lpfc_mbx_post_uembed_sgl_page1 *sgl;
	struct sgl_page_pairs *sgl_pg_pairs;
	void *viraddr;
	LPFC_MBOXQ_t *mbox;
	uint32_t reqlen, alloclen, pg_pairs;
	uint32_t mbox_tmo;
	uint16_t xritag_start = 0;
	int rc = 0;
	uint32_t shdr_status, shdr_add_status;
	dma_addr_t pdma_phys_bpl1;
	union lpfc_sli4_cfg_shdr *shdr;

	/* Calculate the requested length of the dma memory */
	reqlen = cnt * sizeof(struct sgl_page_pairs) +
		 sizeof(union lpfc_sli4_cfg_shdr) + sizeof(uint32_t);
	if (reqlen > SLI4_PAGE_SIZE) {
		lpfc_printf_log(phba, KERN_WARNING, LOG_INIT,
				"0217 Block sgl registration required DMA "
				"size (%d) great than a page\n", reqlen);
		return -ENOMEM;
	}
	mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"0283 Failed to allocate mbox cmd memory\n");
		return -ENOMEM;
	}

	/* Allocate DMA memory and set up the non-embedded mailbox command */
	alloclen = lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_FCOE,
				LPFC_MBOX_OPCODE_FCOE_POST_SGL_PAGES, reqlen,
				LPFC_SLI4_MBX_NEMBED);

	if (alloclen < reqlen) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2561 Allocated DMA memory size (%d) is "
				"less than the requested DMA memory "
				"size (%d)\n", alloclen, reqlen);
		lpfc_sli4_mbox_cmd_free(phba, mbox);
		return -ENOMEM;
	}
	/* Get the first SGE entry from the non-embedded DMA memory */
	viraddr = mbox->sge_array->addr[0];

	/* Set up the SGL pages in the non-embedded DMA pages */
	sgl = (struct lpfc_mbx_post_uembed_sgl_page1 *)viraddr;
	sgl_pg_pairs = &sgl->sgl_pg_pairs;

	pg_pairs = 0;
	list_for_each_entry(psb, sblist, list) {
		/* Set up the sge entry */
		sgl_pg_pairs->sgl_pg0_addr_lo =
			cpu_to_le32(putPaddrLow(psb->dma_phys_bpl));
		sgl_pg_pairs->sgl_pg0_addr_hi =
			cpu_to_le32(putPaddrHigh(psb->dma_phys_bpl));
		if (phba->cfg_sg_dma_buf_size > SGL_PAGE_SIZE)
			pdma_phys_bpl1 = psb->dma_phys_bpl + SGL_PAGE_SIZE;
		else
			pdma_phys_bpl1 = 0;
		sgl_pg_pairs->sgl_pg1_addr_lo =
			cpu_to_le32(putPaddrLow(pdma_phys_bpl1));
		sgl_pg_pairs->sgl_pg1_addr_hi =
			cpu_to_le32(putPaddrHigh(pdma_phys_bpl1));
		/* Keep the first xritag on the list */
		if (pg_pairs == 0)
			xritag_start = psb->cur_iocbq.sli4_xritag;
		sgl_pg_pairs++;
		pg_pairs++;
	}
	bf_set(lpfc_post_sgl_pages_xri, sgl, xritag_start);
	bf_set(lpfc_post_sgl_pages_xricnt, sgl, pg_pairs);
	/* Perform endian conversion if necessary */
	sgl->word0 = cpu_to_le32(sgl->word0);

	if (!phba->sli4_hba.intr_enable)
		rc = lpfc_sli_issue_mbox(phba, mbox, MBX_POLL);
	else {
		mbox_tmo = lpfc_mbox_tmo_val(phba, MBX_SLI4_CONFIG);
		rc = lpfc_sli_issue_mbox_wait(phba, mbox, mbox_tmo);
	}
	shdr = (union lpfc_sli4_cfg_shdr *) &sgl->cfg_shdr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (rc != MBX_TIMEOUT)
		lpfc_sli4_mbox_cmd_free(phba, mbox);
	if (shdr_status || shdr_add_status || rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"2564 POST_SGL_BLOCK mailbox command failed "
				"status x%x add_status x%x mbx status x%x\n",
				shdr_status, shdr_add_status, rc);
		rc = -ENXIO;
	}
	return rc;
}

static int
lpfc_fc_frame_check(struct lpfc_hba *phba, struct fc_frame_header *fc_hdr)
{
	char *rctl_names[] = FC_RCTL_NAMES_INIT;
	char *type_names[] = FC_TYPE_NAMES_INIT;
	struct fc_vft_header *fc_vft_hdr;

	switch (fc_hdr->fh_r_ctl) {
	case FC_RCTL_DD_UNCAT:		/* uncategorized information */
	case FC_RCTL_DD_SOL_DATA:	/* solicited data */
	case FC_RCTL_DD_UNSOL_CTL:	/* unsolicited control */
	case FC_RCTL_DD_SOL_CTL:	/* solicited control or reply */
	case FC_RCTL_DD_UNSOL_DATA:	/* unsolicited data */
	case FC_RCTL_DD_DATA_DESC:	/* data descriptor */
	case FC_RCTL_DD_UNSOL_CMD:	/* unsolicited command */
	case FC_RCTL_DD_CMD_STATUS:	/* command status */
	case FC_RCTL_ELS_REQ:	/* extended link services request */
	case FC_RCTL_ELS_REP:	/* extended link services reply */
	case FC_RCTL_ELS4_REQ:	/* FC-4 ELS request */
	case FC_RCTL_ELS4_REP:	/* FC-4 ELS reply */
	case FC_RCTL_BA_NOP:  	/* basic link service NOP */
	case FC_RCTL_BA_ABTS: 	/* basic link service abort */
	case FC_RCTL_BA_RMC: 	/* remove connection */
	case FC_RCTL_BA_ACC:	/* basic accept */
	case FC_RCTL_BA_RJT:	/* basic reject */
	case FC_RCTL_BA_PRMT:
	case FC_RCTL_ACK_1:	/* acknowledge_1 */
	case FC_RCTL_ACK_0:	/* acknowledge_0 */
	case FC_RCTL_P_RJT:	/* port reject */
	case FC_RCTL_F_RJT:	/* fabric reject */
	case FC_RCTL_P_BSY:	/* port busy */
	case FC_RCTL_F_BSY:	/* fabric busy to data frame */
	case FC_RCTL_F_BSYL:	/* fabric busy to link control frame */
	case FC_RCTL_LCR:	/* link credit reset */
	case FC_RCTL_END:	/* end */
		break;
	case FC_RCTL_VFTH:	/* Virtual Fabric tagging Header */
		fc_vft_hdr = (struct fc_vft_header *)fc_hdr;
		fc_hdr = &((struct fc_frame_header *)fc_vft_hdr)[1];
		return lpfc_fc_frame_check(phba, fc_hdr);
	default:
		goto drop;
	}
	switch (fc_hdr->fh_type) {
	case FC_TYPE_BLS:
	case FC_TYPE_ELS:
	case FC_TYPE_FCP:
	case FC_TYPE_CT:
		break;
	case FC_TYPE_IP:
	case FC_TYPE_ILS:
	default:
		goto drop;
	}
	lpfc_printf_log(phba, KERN_INFO, LOG_ELS,
			"2538 Received frame rctl:%s type:%s\n",
			rctl_names[fc_hdr->fh_r_ctl],
			type_names[fc_hdr->fh_type]);
	return 0;
drop:
	lpfc_printf_log(phba, KERN_WARNING, LOG_ELS,
			"2539 Dropped frame rctl:%s type:%s\n",
			rctl_names[fc_hdr->fh_r_ctl],
			type_names[fc_hdr->fh_type]);
	return 1;
}

static uint32_t
lpfc_fc_hdr_get_vfi(struct fc_frame_header *fc_hdr)
{
	struct fc_vft_header *fc_vft_hdr = (struct fc_vft_header *)fc_hdr;

	if (fc_hdr->fh_r_ctl != FC_RCTL_VFTH)
		return 0;
	return bf_get(fc_vft_hdr_vf_id, fc_vft_hdr);
}

static struct lpfc_vport *
lpfc_fc_frame_to_vport(struct lpfc_hba *phba, struct fc_frame_header *fc_hdr,
		       uint16_t fcfi)
{
	struct lpfc_vport **vports;
	struct lpfc_vport *vport = NULL;
	int i;
	uint32_t did = (fc_hdr->fh_d_id[0] << 16 |
			fc_hdr->fh_d_id[1] << 8 |
			fc_hdr->fh_d_id[2]);

	vports = lpfc_create_vport_work_array(phba);
	if (vports != NULL)
		for (i = 0; i <= phba->max_vpi && vports[i] != NULL; i++) {
			if (phba->fcf.fcfi == fcfi &&
			    vports[i]->vfi == lpfc_fc_hdr_get_vfi(fc_hdr) &&
			    vports[i]->fc_myDID == did) {
				vport = vports[i];
				break;
			}
		}
	lpfc_destroy_vport_work_array(phba, vports);
	return vport;
}

void
lpfc_update_rcv_time_stamp(struct lpfc_vport *vport)
{
	struct lpfc_dmabuf *h_buf;
	struct hbq_dmabuf *dmabuf = NULL;

	/* get the oldest sequence on the rcv list */
	h_buf = list_get_first(&vport->rcv_buffer_list,
			       struct lpfc_dmabuf, list);
	if (!h_buf)
		return;
	dmabuf = container_of(h_buf, struct hbq_dmabuf, hbuf);
	vport->rcv_buffer_time_stamp = dmabuf->time_stamp;
}

void
lpfc_cleanup_rcv_buffers(struct lpfc_vport *vport)
{
	struct lpfc_dmabuf *h_buf, *hnext;
	struct lpfc_dmabuf *d_buf, *dnext;
	struct hbq_dmabuf *dmabuf = NULL;

	/* start with the oldest sequence on the rcv list */
	list_for_each_entry_safe(h_buf, hnext, &vport->rcv_buffer_list, list) {
		dmabuf = container_of(h_buf, struct hbq_dmabuf, hbuf);
		list_del_init(&dmabuf->hbuf.list);
		list_for_each_entry_safe(d_buf, dnext,
					 &dmabuf->dbuf.list, list) {
			list_del_init(&d_buf->list);
			lpfc_in_buf_free(vport->phba, d_buf);
		}
		lpfc_in_buf_free(vport->phba, &dmabuf->dbuf);
	}
}

void
lpfc_rcv_seq_check_edtov(struct lpfc_vport *vport)
{
	struct lpfc_dmabuf *h_buf, *hnext;
	struct lpfc_dmabuf *d_buf, *dnext;
	struct hbq_dmabuf *dmabuf = NULL;
	unsigned long timeout;
	int abort_count = 0;

	timeout = (msecs_to_jiffies(vport->phba->fc_edtov) +
		   vport->rcv_buffer_time_stamp);
	if (list_empty(&vport->rcv_buffer_list) ||
	    time_before(jiffies, timeout))
		return;
	/* start with the oldest sequence on the rcv list */
	list_for_each_entry_safe(h_buf, hnext, &vport->rcv_buffer_list, list) {
		dmabuf = container_of(h_buf, struct hbq_dmabuf, hbuf);
		timeout = (msecs_to_jiffies(vport->phba->fc_edtov) +
			   dmabuf->time_stamp);
		if (time_before(jiffies, timeout))
			break;
		abort_count++;
		list_del_init(&dmabuf->hbuf.list);
		list_for_each_entry_safe(d_buf, dnext,
					 &dmabuf->dbuf.list, list) {
			list_del_init(&d_buf->list);
			lpfc_in_buf_free(vport->phba, d_buf);
		}
		lpfc_in_buf_free(vport->phba, &dmabuf->dbuf);
	}
	if (abort_count)
		lpfc_update_rcv_time_stamp(vport);
}

static struct hbq_dmabuf *
lpfc_fc_frame_add(struct lpfc_vport *vport, struct hbq_dmabuf *dmabuf)
{
	struct fc_frame_header *new_hdr;
	struct fc_frame_header *temp_hdr;
	struct lpfc_dmabuf *d_buf;
	struct lpfc_dmabuf *h_buf;
	struct hbq_dmabuf *seq_dmabuf = NULL;
	struct hbq_dmabuf *temp_dmabuf = NULL;

	INIT_LIST_HEAD(&dmabuf->dbuf.list);
	dmabuf->time_stamp = jiffies;
	new_hdr = (struct fc_frame_header *)dmabuf->hbuf.virt;
	/* Use the hdr_buf to find the sequence that this frame belongs to */
	list_for_each_entry(h_buf, &vport->rcv_buffer_list, list) {
		temp_hdr = (struct fc_frame_header *)h_buf->virt;
		if ((temp_hdr->fh_seq_id != new_hdr->fh_seq_id) ||
		    (temp_hdr->fh_ox_id != new_hdr->fh_ox_id) ||
		    (memcmp(&temp_hdr->fh_s_id, &new_hdr->fh_s_id, 3)))
			continue;
		/* found a pending sequence that matches this frame */
		seq_dmabuf = container_of(h_buf, struct hbq_dmabuf, hbuf);
		break;
	}
	if (!seq_dmabuf) {
		/*
		 * This indicates first frame received for this sequence.
		 * Queue the buffer on the vport's rcv_buffer_list.
		 */
		list_add_tail(&dmabuf->hbuf.list, &vport->rcv_buffer_list);
		lpfc_update_rcv_time_stamp(vport);
		return dmabuf;
	}
	temp_hdr = seq_dmabuf->hbuf.virt;
	if (be16_to_cpu(new_hdr->fh_seq_cnt) <
		be16_to_cpu(temp_hdr->fh_seq_cnt)) {
		list_del_init(&seq_dmabuf->hbuf.list);
		list_add_tail(&dmabuf->hbuf.list, &vport->rcv_buffer_list);
		list_add_tail(&dmabuf->dbuf.list, &seq_dmabuf->dbuf.list);
		lpfc_update_rcv_time_stamp(vport);
		return dmabuf;
	}
	/* move this sequence to the tail to indicate a young sequence */
	list_move_tail(&seq_dmabuf->hbuf.list, &vport->rcv_buffer_list);
	seq_dmabuf->time_stamp = jiffies;
	lpfc_update_rcv_time_stamp(vport);
	if (list_empty(&seq_dmabuf->dbuf.list)) {
		temp_hdr = dmabuf->hbuf.virt;
		list_add_tail(&dmabuf->dbuf.list, &seq_dmabuf->dbuf.list);
		return seq_dmabuf;
	}
	/* find the correct place in the sequence to insert this frame */
	list_for_each_entry_reverse(d_buf, &seq_dmabuf->dbuf.list, list) {
		temp_dmabuf = container_of(d_buf, struct hbq_dmabuf, dbuf);
		temp_hdr = (struct fc_frame_header *)temp_dmabuf->hbuf.virt;
		/*
		 * If the frame's sequence count is greater than the frame on
		 * the list then insert the frame right after this frame
		 */
		if (be16_to_cpu(new_hdr->fh_seq_cnt) >
			be16_to_cpu(temp_hdr->fh_seq_cnt)) {
			list_add(&dmabuf->dbuf.list, &temp_dmabuf->dbuf.list);
			return seq_dmabuf;
		}
	}
	return NULL;
}

static bool
lpfc_sli4_abort_partial_seq(struct lpfc_vport *vport,
			    struct hbq_dmabuf *dmabuf)
{
	struct fc_frame_header *new_hdr;
	struct fc_frame_header *temp_hdr;
	struct lpfc_dmabuf *d_buf, *n_buf, *h_buf;
	struct hbq_dmabuf *seq_dmabuf = NULL;

	/* Use the hdr_buf to find the sequence that matches this frame */
	INIT_LIST_HEAD(&dmabuf->dbuf.list);
	INIT_LIST_HEAD(&dmabuf->hbuf.list);
	new_hdr = (struct fc_frame_header *)dmabuf->hbuf.virt;
	list_for_each_entry(h_buf, &vport->rcv_buffer_list, list) {
		temp_hdr = (struct fc_frame_header *)h_buf->virt;
		if ((temp_hdr->fh_seq_id != new_hdr->fh_seq_id) ||
		    (temp_hdr->fh_ox_id != new_hdr->fh_ox_id) ||
		    (memcmp(&temp_hdr->fh_s_id, &new_hdr->fh_s_id, 3)))
			continue;
		/* found a pending sequence that matches this frame */
		seq_dmabuf = container_of(h_buf, struct hbq_dmabuf, hbuf);
		break;
	}

	/* Free up all the frames from the partially assembled sequence */
	if (seq_dmabuf) {
		list_for_each_entry_safe(d_buf, n_buf,
					 &seq_dmabuf->dbuf.list, list) {
			list_del_init(&d_buf->list);
			lpfc_in_buf_free(vport->phba, d_buf);
		}
		return true;
	}
	return false;
}

static void
lpfc_sli4_seq_abort_acc_cmpl(struct lpfc_hba *phba,
			     struct lpfc_iocbq *cmd_iocbq,
			     struct lpfc_iocbq *rsp_iocbq)
{
	if (cmd_iocbq)
		lpfc_sli_release_iocbq(phba, cmd_iocbq);
}

static void
lpfc_sli4_seq_abort_acc(struct lpfc_hba *phba,
			struct fc_frame_header *fc_hdr)
{
	struct lpfc_iocbq *ctiocb = NULL;
	struct lpfc_nodelist *ndlp;
	uint16_t oxid, rxid;
	uint32_t sid, fctl;
	IOCB_t *icmd;

	if (!lpfc_is_link_up(phba))
		return;

	sid = sli4_sid_from_fc_hdr(fc_hdr);
	oxid = be16_to_cpu(fc_hdr->fh_ox_id);
	rxid = be16_to_cpu(fc_hdr->fh_rx_id);

	ndlp = lpfc_findnode_did(phba->pport, sid);
	if (!ndlp) {
		lpfc_printf_log(phba, KERN_WARNING, LOG_ELS,
				"1268 Find ndlp returned NULL for oxid:x%x "
				"SID:x%x\n", oxid, sid);
		return;
	}

	/* Allocate buffer for acc iocb */
	ctiocb = lpfc_sli_get_iocbq(phba);
	if (!ctiocb)
		return;

	/* Extract the F_CTL field from FC_HDR */
	fctl = sli4_fctl_from_fc_hdr(fc_hdr);

	icmd = &ctiocb->iocb;
	icmd->un.xseq64.bdl.bdeSize = 0;
	icmd->un.xseq64.bdl.ulpIoTag32 = 0;
	icmd->un.xseq64.w5.hcsw.Dfctl = 0;
	icmd->un.xseq64.w5.hcsw.Rctl = FC_RCTL_BA_ACC;
	icmd->un.xseq64.w5.hcsw.Type = FC_TYPE_BLS;

	/* Fill in the rest of iocb fields */
	icmd->ulpCommand = CMD_XMIT_BLS_RSP64_CX;
	icmd->ulpBdeCount = 0;
	icmd->ulpLe = 1;
	icmd->ulpClass = CLASS3;
	icmd->ulpContext = ndlp->nlp_rpi;

	ctiocb->iocb_cmpl = NULL;
	ctiocb->vport = phba->pport;
	ctiocb->iocb_cmpl = lpfc_sli4_seq_abort_acc_cmpl;

	if (fctl & FC_FC_EX_CTX) {
		/* ABTS sent by responder to CT exchange, construction
		 * of BA_ACC will use OX_ID from ABTS for the XRI_TAG
		 * field and RX_ID from ABTS for RX_ID field.
		 */
		bf_set(lpfc_abts_orig, &icmd->un.bls_acc, LPFC_ABTS_UNSOL_RSP);
		bf_set(lpfc_abts_rxid, &icmd->un.bls_acc, rxid);
		ctiocb->sli4_xritag = oxid;
	} else {
		/* ABTS sent by initiator to CT exchange, construction
		 * of BA_ACC will need to allocate a new XRI as for the
		 * XRI_TAG and RX_ID fields.
		 */
		bf_set(lpfc_abts_orig, &icmd->un.bls_acc, LPFC_ABTS_UNSOL_INT);
		bf_set(lpfc_abts_rxid, &icmd->un.bls_acc, NO_XRI);
		ctiocb->sli4_xritag = NO_XRI;
	}
	bf_set(lpfc_abts_oxid, &icmd->un.bls_acc, oxid);

	/* Xmit CT abts accept on exchange <xid> */
	lpfc_printf_log(phba, KERN_INFO, LOG_ELS,
			"1200 Xmit CT ABTS ACC on exchange x%x Data: x%x\n",
			CMD_XMIT_BLS_RSP64_CX, phba->link_state);
	lpfc_sli_issue_iocb(phba, LPFC_ELS_RING, ctiocb, 0);
}

void
lpfc_sli4_handle_unsol_abort(struct lpfc_vport *vport,
			     struct hbq_dmabuf *dmabuf)
{
	struct lpfc_hba *phba = vport->phba;
	struct fc_frame_header fc_hdr;
	uint32_t fctl;
	bool abts_par;

	/* Make a copy of fc_hdr before the dmabuf being released */
	memcpy(&fc_hdr, dmabuf->hbuf.virt, sizeof(struct fc_frame_header));
	fctl = sli4_fctl_from_fc_hdr(&fc_hdr);

	if (fctl & FC_FC_EX_CTX) {
		/*
		 * ABTS sent by responder to exchange, just free the buffer
		 */
		lpfc_in_buf_free(phba, &dmabuf->dbuf);
	} else {
		/*
		 * ABTS sent by initiator to exchange, need to do cleanup
		 */
		/* Try to abort partially assembled seq */
		abts_par = lpfc_sli4_abort_partial_seq(vport, dmabuf);

		/* Send abort to ULP if partially seq abort failed */
		if (abts_par == false)
			lpfc_sli4_send_seq_to_ulp(vport, dmabuf);
		else
			lpfc_in_buf_free(phba, &dmabuf->dbuf);
	}
	/* Send basic accept (BA_ACC) to the abort requester */
	lpfc_sli4_seq_abort_acc(phba, &fc_hdr);
}

static int
lpfc_seq_complete(struct hbq_dmabuf *dmabuf)
{
	struct fc_frame_header *hdr;
	struct lpfc_dmabuf *d_buf;
	struct hbq_dmabuf *seq_dmabuf;
	uint32_t fctl;
	int seq_count = 0;

	hdr = (struct fc_frame_header *)dmabuf->hbuf.virt;
	/* make sure first fame of sequence has a sequence count of zero */
	if (hdr->fh_seq_cnt != seq_count)
		return 0;
	fctl = (hdr->fh_f_ctl[0] << 16 |
		hdr->fh_f_ctl[1] << 8 |
		hdr->fh_f_ctl[2]);
	/* If last frame of sequence we can return success. */
	if (fctl & FC_FC_END_SEQ)
		return 1;
	list_for_each_entry(d_buf, &dmabuf->dbuf.list, list) {
		seq_dmabuf = container_of(d_buf, struct hbq_dmabuf, dbuf);
		hdr = (struct fc_frame_header *)seq_dmabuf->hbuf.virt;
		/* If there is a hole in the sequence count then fail. */
		if (++seq_count != be16_to_cpu(hdr->fh_seq_cnt))
			return 0;
		fctl = (hdr->fh_f_ctl[0] << 16 |
			hdr->fh_f_ctl[1] << 8 |
			hdr->fh_f_ctl[2]);
		/* If last frame of sequence we can return success. */
		if (fctl & FC_FC_END_SEQ)
			return 1;
	}
	return 0;
}

static struct lpfc_iocbq *
lpfc_prep_seq(struct lpfc_vport *vport, struct hbq_dmabuf *seq_dmabuf)
{
	struct lpfc_dmabuf *d_buf, *n_buf;
	struct lpfc_iocbq *first_iocbq, *iocbq;
	struct fc_frame_header *fc_hdr;
	uint32_t sid;
	struct ulp_bde64 *pbde;

	fc_hdr = (struct fc_frame_header *)seq_dmabuf->hbuf.virt;
	/* remove from receive buffer list */
	list_del_init(&seq_dmabuf->hbuf.list);
	lpfc_update_rcv_time_stamp(vport);
	/* get the Remote Port's SID */
	sid = sli4_sid_from_fc_hdr(fc_hdr);
	/* Get an iocbq struct to fill in. */
	first_iocbq = lpfc_sli_get_iocbq(vport->phba);
	if (first_iocbq) {
		/* Initialize the first IOCB. */
		first_iocbq->iocb.unsli3.rcvsli3.acc_len = 0;
		first_iocbq->iocb.ulpStatus = IOSTAT_SUCCESS;
		first_iocbq->iocb.ulpCommand = CMD_IOCB_RCV_SEQ64_CX;
		first_iocbq->iocb.ulpContext = be16_to_cpu(fc_hdr->fh_ox_id);
		first_iocbq->iocb.unsli3.rcvsli3.vpi =
					vport->vpi + vport->phba->vpi_base;
		/* put the first buffer into the first IOCBq */
		first_iocbq->context2 = &seq_dmabuf->dbuf;
		first_iocbq->context3 = NULL;
		first_iocbq->iocb.ulpBdeCount = 1;
		first_iocbq->iocb.un.cont64[0].tus.f.bdeSize =
							LPFC_DATA_BUF_SIZE;
		first_iocbq->iocb.un.rcvels.remoteID = sid;
		first_iocbq->iocb.unsli3.rcvsli3.acc_len +=
				bf_get(lpfc_rcqe_length,
				       &seq_dmabuf->cq_event.cqe.rcqe_cmpl);
	}
	iocbq = first_iocbq;
	/*
	 * Each IOCBq can have two Buffers assigned, so go through the list
	 * of buffers for this sequence and save two buffers in each IOCBq
	 */
	list_for_each_entry_safe(d_buf, n_buf, &seq_dmabuf->dbuf.list, list) {
		if (!iocbq) {
			lpfc_in_buf_free(vport->phba, d_buf);
			continue;
		}
		if (!iocbq->context3) {
			iocbq->context3 = d_buf;
			iocbq->iocb.ulpBdeCount++;
			pbde = (struct ulp_bde64 *)
					&iocbq->iocb.unsli3.sli3Words[4];
			pbde->tus.f.bdeSize = LPFC_DATA_BUF_SIZE;
			first_iocbq->iocb.unsli3.rcvsli3.acc_len +=
				bf_get(lpfc_rcqe_length,
				       &seq_dmabuf->cq_event.cqe.rcqe_cmpl);
		} else {
			iocbq = lpfc_sli_get_iocbq(vport->phba);
			if (!iocbq) {
				if (first_iocbq) {
					first_iocbq->iocb.ulpStatus =
							IOSTAT_FCP_RSP_ERROR;
					first_iocbq->iocb.un.ulpWord[4] =
							IOERR_NO_RESOURCES;
				}
				lpfc_in_buf_free(vport->phba, d_buf);
				continue;
			}
			iocbq->context2 = d_buf;
			iocbq->context3 = NULL;
			iocbq->iocb.ulpBdeCount = 1;
			iocbq->iocb.un.cont64[0].tus.f.bdeSize =
							LPFC_DATA_BUF_SIZE;
			first_iocbq->iocb.unsli3.rcvsli3.acc_len +=
				bf_get(lpfc_rcqe_length,
				       &seq_dmabuf->cq_event.cqe.rcqe_cmpl);
			iocbq->iocb.un.rcvels.remoteID = sid;
			list_add_tail(&iocbq->list, &first_iocbq->list);
		}
	}
	return first_iocbq;
}

static void
lpfc_sli4_send_seq_to_ulp(struct lpfc_vport *vport,
			  struct hbq_dmabuf *seq_dmabuf)
{
	struct fc_frame_header *fc_hdr;
	struct lpfc_iocbq *iocbq, *curr_iocb, *next_iocb;
	struct lpfc_hba *phba = vport->phba;

	fc_hdr = (struct fc_frame_header *)seq_dmabuf->hbuf.virt;
	iocbq = lpfc_prep_seq(vport, seq_dmabuf);
	if (!iocbq) {
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"2707 Ring %d handler: Failed to allocate "
				"iocb Rctl x%x Type x%x received\n",
				LPFC_ELS_RING,
				fc_hdr->fh_r_ctl, fc_hdr->fh_type);
		return;
	}
	if (!lpfc_complete_unsol_iocb(phba,
				      &phba->sli.ring[LPFC_ELS_RING],
				      iocbq, fc_hdr->fh_r_ctl,
				      fc_hdr->fh_type))
		lpfc_printf_log(phba, KERN_WARNING, LOG_SLI,
				"2540 Ring %d handler: unexpected Rctl "
				"x%x Type x%x received\n",
				LPFC_ELS_RING,
				fc_hdr->fh_r_ctl, fc_hdr->fh_type);

	/* Free iocb created in lpfc_prep_seq */
	list_for_each_entry_safe(curr_iocb, next_iocb,
		&iocbq->list, list) {
		list_del_init(&curr_iocb->list);
		lpfc_sli_release_iocbq(phba, curr_iocb);
	}
	lpfc_sli_release_iocbq(phba, iocbq);
}

void
lpfc_sli4_handle_received_buffer(struct lpfc_hba *phba,
				 struct hbq_dmabuf *dmabuf)
{
	struct hbq_dmabuf *seq_dmabuf;
	struct fc_frame_header *fc_hdr;
	struct lpfc_vport *vport;
	uint32_t fcfi;

	/* Process each received buffer */
	fc_hdr = (struct fc_frame_header *)dmabuf->hbuf.virt;
	/* check to see if this a valid type of frame */
	if (lpfc_fc_frame_check(phba, fc_hdr)) {
		lpfc_in_buf_free(phba, &dmabuf->dbuf);
		return;
	}
	fcfi = bf_get(lpfc_rcqe_fcf_id, &dmabuf->cq_event.cqe.rcqe_cmpl);
	vport = lpfc_fc_frame_to_vport(phba, fc_hdr, fcfi);
	if (!vport || !(vport->vpi_state & LPFC_VPI_REGISTERED)) {
		/* throw out the frame */
		lpfc_in_buf_free(phba, &dmabuf->dbuf);
		return;
	}
	/* Handle the basic abort sequence (BA_ABTS) event */
	if (fc_hdr->fh_r_ctl == FC_RCTL_BA_ABTS) {
		lpfc_sli4_handle_unsol_abort(vport, dmabuf);
		return;
	}

	/* Link this frame */
	seq_dmabuf = lpfc_fc_frame_add(vport, dmabuf);
	if (!seq_dmabuf) {
		/* unable to add frame to vport - throw it out */
		lpfc_in_buf_free(phba, &dmabuf->dbuf);
		return;
	}
	/* If not last frame in sequence continue processing frames. */
	if (!lpfc_seq_complete(seq_dmabuf))
		return;

	/* Send the complete sequence to the upper layer protocol */
	lpfc_sli4_send_seq_to_ulp(vport, seq_dmabuf);
}

int
lpfc_sli4_post_all_rpi_hdrs(struct lpfc_hba *phba)
{
	struct lpfc_rpi_hdr *rpi_page;
	uint32_t rc = 0;

	/* Post all rpi memory regions to the port. */
	list_for_each_entry(rpi_page, &phba->sli4_hba.lpfc_rpi_hdr_list, list) {
		rc = lpfc_sli4_post_rpi_hdr(phba, rpi_page);
		if (rc != MBX_SUCCESS) {
			lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
					"2008 Error %d posting all rpi "
					"headers\n", rc);
			rc = -EIO;
			break;
		}
	}

	return rc;
}

int
lpfc_sli4_post_rpi_hdr(struct lpfc_hba *phba, struct lpfc_rpi_hdr *rpi_page)
{
	LPFC_MBOXQ_t *mboxq;
	struct lpfc_mbx_post_hdr_tmpl *hdr_tmpl;
	uint32_t rc = 0;
	uint32_t mbox_tmo;
	uint32_t shdr_status, shdr_add_status;
	union lpfc_sli4_cfg_shdr *shdr;

	/* The port is notified of the header region via a mailbox command. */
	mboxq = (LPFC_MBOXQ_t *) mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mboxq) {
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"2001 Unable to allocate memory for issuing "
				"SLI_CONFIG_SPECIAL mailbox command\n");
		return -ENOMEM;
	}

	/* Post all rpi memory regions to the port. */
	hdr_tmpl = &mboxq->u.mqe.un.hdr_tmpl;
	mbox_tmo = lpfc_mbox_tmo_val(phba, MBX_SLI4_CONFIG);
	lpfc_sli4_config(phba, mboxq, LPFC_MBOX_SUBSYSTEM_FCOE,
			 LPFC_MBOX_OPCODE_FCOE_POST_HDR_TEMPLATE,
			 sizeof(struct lpfc_mbx_post_hdr_tmpl) -
			 sizeof(struct mbox_header), LPFC_SLI4_MBX_EMBED);
	bf_set(lpfc_mbx_post_hdr_tmpl_page_cnt,
	       hdr_tmpl, rpi_page->page_count);
	bf_set(lpfc_mbx_post_hdr_tmpl_rpi_offset, hdr_tmpl,
	       rpi_page->start_rpi);
	hdr_tmpl->rpi_paddr_lo = putPaddrLow(rpi_page->dmabuf->phys);
	hdr_tmpl->rpi_paddr_hi = putPaddrHigh(rpi_page->dmabuf->phys);
	rc = lpfc_sli_issue_mbox(phba, mboxq, MBX_POLL);
	shdr = (union lpfc_sli4_cfg_shdr *) &hdr_tmpl->header.cfg_shdr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);
	if (rc != MBX_TIMEOUT)
		mempool_free(mboxq, phba->mbox_mem_pool);
	if (shdr_status || shdr_add_status || rc) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2514 POST_RPI_HDR mailbox failed with "
				"status x%x add_status x%x, mbx status x%x\n",
				shdr_status, shdr_add_status, rc);
		rc = -ENXIO;
	}
	return rc;
}

int
lpfc_sli4_alloc_rpi(struct lpfc_hba *phba)
{
	int rpi;
	uint16_t max_rpi, rpi_base, rpi_limit;
	uint16_t rpi_remaining;
	struct lpfc_rpi_hdr *rpi_hdr;

	max_rpi = phba->sli4_hba.max_cfg_param.max_rpi;
	rpi_base = phba->sli4_hba.max_cfg_param.rpi_base;
	rpi_limit = phba->sli4_hba.next_rpi;

	/*
	 * The valid rpi range is not guaranteed to be zero-based.  Start
	 * the search at the rpi_base as reported by the port.
	 */
	spin_lock_irq(&phba->hbalock);
	rpi = find_next_zero_bit(phba->sli4_hba.rpi_bmask, rpi_limit, rpi_base);
	if (rpi >= rpi_limit || rpi < rpi_base)
		rpi = LPFC_RPI_ALLOC_ERROR;
	else {
		set_bit(rpi, phba->sli4_hba.rpi_bmask);
		phba->sli4_hba.max_cfg_param.rpi_used++;
		phba->sli4_hba.rpi_count++;
	}

	/*
	 * Don't try to allocate more rpi header regions if the device limit
	 * on available rpis max has been exhausted.
	 */
	if ((rpi == LPFC_RPI_ALLOC_ERROR) &&
	    (phba->sli4_hba.rpi_count >= max_rpi)) {
		spin_unlock_irq(&phba->hbalock);
		return rpi;
	}

	/*
	 * If the driver is running low on rpi resources, allocate another
	 * page now.  Note that the next_rpi value is used because
	 * it represents how many are actually in use whereas max_rpi notes
	 * how many are supported max by the device.
	 */
	rpi_remaining = phba->sli4_hba.next_rpi - rpi_base -
		phba->sli4_hba.rpi_count;
	spin_unlock_irq(&phba->hbalock);
	if (rpi_remaining < LPFC_RPI_LOW_WATER_MARK) {
		rpi_hdr = lpfc_sli4_create_rpi_hdr(phba);
		if (!rpi_hdr) {
			lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
					"2002 Error Could not grow rpi "
					"count\n");
		} else {
			lpfc_sli4_post_rpi_hdr(phba, rpi_hdr);
		}
	}

	return rpi;
}

void
lpfc_sli4_free_rpi(struct lpfc_hba *phba, int rpi)
{
	spin_lock_irq(&phba->hbalock);
	clear_bit(rpi, phba->sli4_hba.rpi_bmask);
	phba->sli4_hba.rpi_count--;
	phba->sli4_hba.max_cfg_param.rpi_used--;
	spin_unlock_irq(&phba->hbalock);
}

void
lpfc_sli4_remove_rpis(struct lpfc_hba *phba)
{
	kfree(phba->sli4_hba.rpi_bmask);
}

int
lpfc_sli4_resume_rpi(struct lpfc_nodelist *ndlp)
{
	LPFC_MBOXQ_t *mboxq;
	struct lpfc_hba *phba = ndlp->phba;
	int rc;

	/* The port is notified of the header region via a mailbox command. */
	mboxq = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mboxq)
		return -ENOMEM;

	/* Post all rpi memory regions to the port. */
	lpfc_resume_rpi(mboxq, ndlp);
	rc = lpfc_sli_issue_mbox(phba, mboxq, MBX_NOWAIT);
	if (rc == MBX_NOT_FINISHED) {
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"2010 Resume RPI Mailbox failed "
				"status %d, mbxStatus x%x\n", rc,
				bf_get(lpfc_mqe_status, &mboxq->u.mqe));
		mempool_free(mboxq, phba->mbox_mem_pool);
		return -EIO;
	}
	return 0;
}

int
lpfc_sli4_init_vpi(struct lpfc_hba *phba, uint16_t vpi)
{
	LPFC_MBOXQ_t *mboxq;
	int rc = 0;
	int retval = MBX_SUCCESS;
	uint32_t mbox_tmo;

	if (vpi == 0)
		return -EINVAL;
	mboxq = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mboxq)
		return -ENOMEM;
	lpfc_init_vpi(phba, mboxq, vpi);
	mbox_tmo = lpfc_mbox_tmo_val(phba, MBX_INIT_VPI);
	rc = lpfc_sli_issue_mbox_wait(phba, mboxq, mbox_tmo);
	if (rc != MBX_SUCCESS) {
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"2022 INIT VPI Mailbox failed "
				"status %d, mbxStatus x%x\n", rc,
				bf_get(lpfc_mqe_status, &mboxq->u.mqe));
		retval = -EIO;
	}
	if (rc != MBX_TIMEOUT)
		mempool_free(mboxq, phba->mbox_mem_pool);

	return retval;
}

static void
lpfc_mbx_cmpl_add_fcf_record(struct lpfc_hba *phba, LPFC_MBOXQ_t *mboxq)
{
	void *virt_addr;
	union lpfc_sli4_cfg_shdr *shdr;
	uint32_t shdr_status, shdr_add_status;

	virt_addr = mboxq->sge_array->addr[0];
	/* The IOCTL status is embedded in the mailbox subheader. */
	shdr = (union lpfc_sli4_cfg_shdr *) virt_addr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status, &shdr->response);

	if ((shdr_status || shdr_add_status) &&
		(shdr_status != STATUS_FCF_IN_USE))
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
			"2558 ADD_FCF_RECORD mailbox failed with "
			"status x%x add_status x%x\n",
			shdr_status, shdr_add_status);

	lpfc_sli4_mbox_cmd_free(phba, mboxq);
}

int
lpfc_sli4_add_fcf_record(struct lpfc_hba *phba, struct fcf_record *fcf_record)
{
	int rc = 0;
	LPFC_MBOXQ_t *mboxq;
	uint8_t *bytep;
	void *virt_addr;
	dma_addr_t phys_addr;
	struct lpfc_mbx_sge sge;
	uint32_t alloc_len, req_len;
	uint32_t fcfindex;

	mboxq = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mboxq) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
			"2009 Failed to allocate mbox for ADD_FCF cmd\n");
		return -ENOMEM;
	}

	req_len = sizeof(struct fcf_record) + sizeof(union lpfc_sli4_cfg_shdr) +
		  sizeof(uint32_t);

	/* Allocate DMA memory and set up the non-embedded mailbox command */
	alloc_len = lpfc_sli4_config(phba, mboxq, LPFC_MBOX_SUBSYSTEM_FCOE,
				     LPFC_MBOX_OPCODE_FCOE_ADD_FCF,
				     req_len, LPFC_SLI4_MBX_NEMBED);
	if (alloc_len < req_len) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
			"2523 Allocated DMA memory size (x%x) is "
			"less than the requested DMA memory "
			"size (x%x)\n", alloc_len, req_len);
		lpfc_sli4_mbox_cmd_free(phba, mboxq);
		return -ENOMEM;
	}

	/*
	 * Get the first SGE entry from the non-embedded DMA memory.  This
	 * routine only uses a single SGE.
	 */
	lpfc_sli4_mbx_sge_get(mboxq, 0, &sge);
	phys_addr = getPaddr(sge.pa_hi, sge.pa_lo);
	virt_addr = mboxq->sge_array->addr[0];
	/*
	 * Configure the FCF record for FCFI 0.  This is the driver's
	 * hardcoded default and gets used in nonFIP mode.
	 */
	fcfindex = bf_get(lpfc_fcf_record_fcf_index, fcf_record);
	bytep = virt_addr + sizeof(union lpfc_sli4_cfg_shdr);
	lpfc_sli_pcimem_bcopy(&fcfindex, bytep, sizeof(uint32_t));

	/*
	 * Copy the fcf_index and the FCF Record Data. The data starts after
	 * the FCoE header plus word10. The data copy needs to be endian
	 * correct.
	 */
	bytep += sizeof(uint32_t);
	lpfc_sli_pcimem_bcopy(fcf_record, bytep, sizeof(struct fcf_record));
	mboxq->vport = phba->pport;
	mboxq->mbox_cmpl = lpfc_mbx_cmpl_add_fcf_record;
	rc = lpfc_sli_issue_mbox(phba, mboxq, MBX_NOWAIT);
	if (rc == MBX_NOT_FINISHED) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
			"2515 ADD_FCF_RECORD mailbox failed with "
			"status 0x%x\n", rc);
		lpfc_sli4_mbox_cmd_free(phba, mboxq);
		rc = -EIO;
	} else
		rc = 0;

	return rc;
}

void
lpfc_sli4_build_dflt_fcf_record(struct lpfc_hba *phba,
				struct fcf_record *fcf_record,
				uint16_t fcf_index)
{
	memset(fcf_record, 0, sizeof(struct fcf_record));
	fcf_record->max_rcv_size = LPFC_FCOE_MAX_RCV_SIZE;
	fcf_record->fka_adv_period = LPFC_FCOE_FKA_ADV_PER;
	fcf_record->fip_priority = LPFC_FCOE_FIP_PRIORITY;
	bf_set(lpfc_fcf_record_mac_0, fcf_record, phba->fc_map[0]);
	bf_set(lpfc_fcf_record_mac_1, fcf_record, phba->fc_map[1]);
	bf_set(lpfc_fcf_record_mac_2, fcf_record, phba->fc_map[2]);
	bf_set(lpfc_fcf_record_mac_3, fcf_record, LPFC_FCOE_FCF_MAC3);
	bf_set(lpfc_fcf_record_mac_4, fcf_record, LPFC_FCOE_FCF_MAC4);
	bf_set(lpfc_fcf_record_mac_5, fcf_record, LPFC_FCOE_FCF_MAC5);
	bf_set(lpfc_fcf_record_fc_map_0, fcf_record, phba->fc_map[0]);
	bf_set(lpfc_fcf_record_fc_map_1, fcf_record, phba->fc_map[1]);
	bf_set(lpfc_fcf_record_fc_map_2, fcf_record, phba->fc_map[2]);
	bf_set(lpfc_fcf_record_fcf_valid, fcf_record, 1);
	bf_set(lpfc_fcf_record_fcf_avail, fcf_record, 1);
	bf_set(lpfc_fcf_record_fcf_index, fcf_record, fcf_index);
	bf_set(lpfc_fcf_record_mac_addr_prov, fcf_record,
		LPFC_FCF_FPMA | LPFC_FCF_SPMA);
	/* Set the VLAN bit map */
	if (phba->valid_vlan) {
		fcf_record->vlan_bitmap[phba->vlan_id / 8]
			= 1 << (phba->vlan_id % 8);
	}
}

int
lpfc_sli4_fcf_scan_read_fcf_rec(struct lpfc_hba *phba, uint16_t fcf_index)
{
	int rc = 0, error;
	LPFC_MBOXQ_t *mboxq;

	phba->fcoe_eventtag_at_fcf_scan = phba->fcoe_eventtag;
	mboxq = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mboxq) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2000 Failed to allocate mbox for "
				"READ_FCF cmd\n");
		error = -ENOMEM;
		goto fail_fcf_scan;
	}
	/* Construct the read FCF record mailbox command */
	rc = lpfc_sli4_mbx_read_fcf_rec(phba, mboxq, fcf_index);
	if (rc) {
		error = -EINVAL;
		goto fail_fcf_scan;
	}
	/* Issue the mailbox command asynchronously */
	mboxq->vport = phba->pport;
	mboxq->mbox_cmpl = lpfc_mbx_cmpl_fcf_scan_read_fcf_rec;
	rc = lpfc_sli_issue_mbox(phba, mboxq, MBX_NOWAIT);
	if (rc == MBX_NOT_FINISHED)
		error = -EIO;
	else {
		spin_lock_irq(&phba->hbalock);
		phba->hba_flag |= FCF_DISC_INPROGRESS;
		spin_unlock_irq(&phba->hbalock);
		/* Reset FCF round robin index bmask for new scan */
		if (fcf_index == LPFC_FCOE_FCF_GET_FIRST) {
			memset(phba->fcf.fcf_rr_bmask, 0,
			       sizeof(*phba->fcf.fcf_rr_bmask));
			phba->fcf.eligible_fcf_cnt = 0;
		}
		error = 0;
	}
fail_fcf_scan:
	if (error) {
		if (mboxq)
			lpfc_sli4_mbox_cmd_free(phba, mboxq);
		/* FCF scan failed, clear FCF_DISC_INPROGRESS flag */
		spin_lock_irq(&phba->hbalock);
		phba->hba_flag &= ~FCF_DISC_INPROGRESS;
		spin_unlock_irq(&phba->hbalock);
	}
	return error;
}

int
lpfc_sli4_fcf_rr_read_fcf_rec(struct lpfc_hba *phba, uint16_t fcf_index)
{
	int rc = 0, error;
	LPFC_MBOXQ_t *mboxq;

	mboxq = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mboxq) {
		lpfc_printf_log(phba, KERN_ERR, LOG_FIP | LOG_INIT,
				"2763 Failed to allocate mbox for "
				"READ_FCF cmd\n");
		error = -ENOMEM;
		goto fail_fcf_read;
	}
	/* Construct the read FCF record mailbox command */
	rc = lpfc_sli4_mbx_read_fcf_rec(phba, mboxq, fcf_index);
	if (rc) {
		error = -EINVAL;
		goto fail_fcf_read;
	}
	/* Issue the mailbox command asynchronously */
	mboxq->vport = phba->pport;
	mboxq->mbox_cmpl = lpfc_mbx_cmpl_fcf_rr_read_fcf_rec;
	rc = lpfc_sli_issue_mbox(phba, mboxq, MBX_NOWAIT);
	if (rc == MBX_NOT_FINISHED)
		error = -EIO;
	else
		error = 0;

fail_fcf_read:
	if (error && mboxq)
		lpfc_sli4_mbox_cmd_free(phba, mboxq);
	return error;
}

int
lpfc_sli4_read_fcf_rec(struct lpfc_hba *phba, uint16_t fcf_index)
{
	int rc = 0, error;
	LPFC_MBOXQ_t *mboxq;

	mboxq = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mboxq) {
		lpfc_printf_log(phba, KERN_ERR, LOG_FIP | LOG_INIT,
				"2758 Failed to allocate mbox for "
				"READ_FCF cmd\n");
				error = -ENOMEM;
				goto fail_fcf_read;
	}
	/* Construct the read FCF record mailbox command */
	rc = lpfc_sli4_mbx_read_fcf_rec(phba, mboxq, fcf_index);
	if (rc) {
		error = -EINVAL;
		goto fail_fcf_read;
	}
	/* Issue the mailbox command asynchronously */
	mboxq->vport = phba->pport;
	mboxq->mbox_cmpl = lpfc_mbx_cmpl_read_fcf_rec;
	rc = lpfc_sli_issue_mbox(phba, mboxq, MBX_NOWAIT);
	if (rc == MBX_NOT_FINISHED)
		error = -EIO;
	else
		error = 0;

fail_fcf_read:
	if (error && mboxq)
		lpfc_sli4_mbox_cmd_free(phba, mboxq);
	return error;
}

uint16_t
lpfc_sli4_fcf_rr_next_index_get(struct lpfc_hba *phba)
{
	uint16_t next_fcf_index;

	/* Search from the currently registered FCF index */
	next_fcf_index = find_next_bit(phba->fcf.fcf_rr_bmask,
				       LPFC_SLI4_FCF_TBL_INDX_MAX,
				       phba->fcf.current_rec.fcf_indx);
	/* Wrap around condition on phba->fcf.fcf_rr_bmask */
	if (next_fcf_index >= LPFC_SLI4_FCF_TBL_INDX_MAX)
		next_fcf_index = find_next_bit(phba->fcf.fcf_rr_bmask,
					       LPFC_SLI4_FCF_TBL_INDX_MAX, 0);
	/* Round robin failover stop condition */
	if (next_fcf_index == phba->fcf.fcf_rr_init_indx)
		return LPFC_FCOE_FCF_NEXT_NONE;

	return next_fcf_index;
}

int
lpfc_sli4_fcf_rr_index_set(struct lpfc_hba *phba, uint16_t fcf_index)
{
	if (fcf_index >= LPFC_SLI4_FCF_TBL_INDX_MAX) {
		lpfc_printf_log(phba, KERN_ERR, LOG_FIP,
				"2610 HBA FCF index reached driver's "
				"book keeping dimension: fcf_index:%d, "
				"driver_bmask_max:%d\n",
				fcf_index, LPFC_SLI4_FCF_TBL_INDX_MAX);
		return -EINVAL;
	}
	/* Set the eligible FCF record index bmask */
	set_bit(fcf_index, phba->fcf.fcf_rr_bmask);

	return 0;
}

void
lpfc_sli4_fcf_rr_index_clear(struct lpfc_hba *phba, uint16_t fcf_index)
{
	if (fcf_index >= LPFC_SLI4_FCF_TBL_INDX_MAX) {
		lpfc_printf_log(phba, KERN_ERR, LOG_FIP,
				"2762 HBA FCF index goes beyond driver's "
				"book keeping dimension: fcf_index:%d, "
				"driver_bmask_max:%d\n",
				fcf_index, LPFC_SLI4_FCF_TBL_INDX_MAX);
		return;
	}
	/* Clear the eligible FCF record index bmask */
	clear_bit(fcf_index, phba->fcf.fcf_rr_bmask);
}

void
lpfc_mbx_cmpl_redisc_fcf_table(struct lpfc_hba *phba, LPFC_MBOXQ_t *mbox)
{
	struct lpfc_mbx_redisc_fcf_tbl *redisc_fcf;
	uint32_t shdr_status, shdr_add_status;

	redisc_fcf = &mbox->u.mqe.un.redisc_fcf_tbl;

	shdr_status = bf_get(lpfc_mbox_hdr_status,
			     &redisc_fcf->header.cfg_shdr.response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status,
			     &redisc_fcf->header.cfg_shdr.response);
	if (shdr_status || shdr_add_status) {
		lpfc_printf_log(phba, KERN_ERR, LOG_FIP,
				"2746 Requesting for FCF rediscovery failed "
				"status x%x add_status x%x\n",
				shdr_status, shdr_add_status);
		if (phba->fcf.fcf_flag & FCF_ACVL_DISC) {
			spin_lock_irq(&phba->hbalock);
			phba->fcf.fcf_flag &= ~FCF_ACVL_DISC;
			spin_unlock_irq(&phba->hbalock);
			/*
			 * CVL event triggered FCF rediscover request failed,
			 * last resort to re-try current registered FCF entry.
			 */
			lpfc_retry_pport_discovery(phba);
		} else {
			spin_lock_irq(&phba->hbalock);
			phba->fcf.fcf_flag &= ~FCF_DEAD_DISC;
			spin_unlock_irq(&phba->hbalock);
			/*
			 * DEAD FCF event triggered FCF rediscover request
			 * failed, last resort to fail over as a link down
			 * to FCF registration.
			 */
			lpfc_sli4_fcf_dead_failthrough(phba);
		}
	} else {
		lpfc_printf_log(phba, KERN_INFO, LOG_FIP,
				"2775 Start FCF rediscovery quiescent period "
				"wait timer before scaning FCF table\n");
		/*
		 * Start FCF rediscovery wait timer for pending FCF
		 * before rescan FCF record table.
		 */
		lpfc_fcf_redisc_wait_start_timer(phba);
	}

	mempool_free(mbox, phba->mbox_mem_pool);
}

int
lpfc_sli4_redisc_fcf_table(struct lpfc_hba *phba)
{
	LPFC_MBOXQ_t *mbox;
	struct lpfc_mbx_redisc_fcf_tbl *redisc_fcf;
	int rc, length;

	/* Cancel retry delay timers to all vports before FCF rediscover */
	lpfc_cancel_all_vport_retry_delay_timer(phba);

	mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox) {
		lpfc_printf_log(phba, KERN_ERR, LOG_SLI,
				"2745 Failed to allocate mbox for "
				"requesting FCF rediscover.\n");
		return -ENOMEM;
	}

	length = (sizeof(struct lpfc_mbx_redisc_fcf_tbl) -
		  sizeof(struct lpfc_sli4_cfg_mhdr));
	lpfc_sli4_config(phba, mbox, LPFC_MBOX_SUBSYSTEM_FCOE,
			 LPFC_MBOX_OPCODE_FCOE_REDISCOVER_FCF,
			 length, LPFC_SLI4_MBX_EMBED);

	redisc_fcf = &mbox->u.mqe.un.redisc_fcf_tbl;
	/* Set count to 0 for invalidating the entire FCF database */
	bf_set(lpfc_mbx_redisc_fcf_count, redisc_fcf, 0);

	/* Issue the mailbox command asynchronously */
	mbox->vport = phba->pport;
	mbox->mbox_cmpl = lpfc_mbx_cmpl_redisc_fcf_table;
	rc = lpfc_sli_issue_mbox(phba, mbox, MBX_NOWAIT);

	if (rc == MBX_NOT_FINISHED) {
		mempool_free(mbox, phba->mbox_mem_pool);
		return -EIO;
	}
	return 0;
}

void
lpfc_sli4_fcf_dead_failthrough(struct lpfc_hba *phba)
{
	uint32_t link_state;

	/*
	 * Last resort as FCF DEAD event failover will treat this as
	 * a link down, but save the link state because we don't want
	 * it to be changed to Link Down unless it is already down.
	 */
	link_state = phba->link_state;
	lpfc_linkdown(phba);
	phba->link_state = link_state;

	/* Unregister FCF if no devices connected to it */
	lpfc_unregister_unused_fcf(phba);
}

void
lpfc_sli_read_link_ste(struct lpfc_hba *phba)
{
	LPFC_MBOXQ_t *pmb = NULL;
	MAILBOX_t *mb;
	uint8_t *rgn23_data = NULL;
	uint32_t offset = 0, data_size, sub_tlv_len, tlv_offset;
	int rc;

	pmb = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!pmb) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
			"2600 lpfc_sli_read_serdes_param failed to"
			" allocate mailbox memory\n");
		goto out;
	}
	mb = &pmb->u.mb;

	/* Get adapter Region 23 data */
	rgn23_data = kzalloc(DMP_RGN23_SIZE, GFP_KERNEL);
	if (!rgn23_data)
		goto out;

	do {
		lpfc_dump_mem(phba, pmb, offset, DMP_REGION_23);
		rc = lpfc_sli_issue_mbox(phba, pmb, MBX_POLL);

		if (rc != MBX_SUCCESS) {
			lpfc_printf_log(phba, KERN_INFO, LOG_INIT,
				"2601 lpfc_sli_read_link_ste failed to"
				" read config region 23 rc 0x%x Status 0x%x\n",
				rc, mb->mbxStatus);
			mb->un.varDmp.word_cnt = 0;
		}
		/*
		 * dump mem may return a zero when finished or we got a
		 * mailbox error, either way we are done.
		 */
		if (mb->un.varDmp.word_cnt == 0)
			break;
		if (mb->un.varDmp.word_cnt > DMP_RGN23_SIZE - offset)
			mb->un.varDmp.word_cnt = DMP_RGN23_SIZE - offset;

		lpfc_sli_pcimem_bcopy(((uint8_t *)mb) + DMP_RSP_OFFSET,
			rgn23_data + offset,
			mb->un.varDmp.word_cnt);
		offset += mb->un.varDmp.word_cnt;
	} while (mb->un.varDmp.word_cnt && offset < DMP_RGN23_SIZE);

	data_size = offset;
	offset = 0;

	if (!data_size)
		goto out;

	/* Check the region signature first */
	if (memcmp(&rgn23_data[offset], LPFC_REGION23_SIGNATURE, 4)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
			"2619 Config region 23 has bad signature\n");
			goto out;
	}
	offset += 4;

	/* Check the data structure version */
	if (rgn23_data[offset] != LPFC_REGION23_VERSION) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
			"2620 Config region 23 has bad version\n");
		goto out;
	}
	offset += 4;

	/* Parse TLV entries in the region */
	while (offset < data_size) {
		if (rgn23_data[offset] == LPFC_REGION23_LAST_REC)
			break;
		/*
		 * If the TLV is not driver specific TLV or driver id is
		 * not linux driver id, skip the record.
		 */
		if ((rgn23_data[offset] != DRIVER_SPECIFIC_TYPE) ||
		    (rgn23_data[offset + 2] != LINUX_DRIVER_ID) ||
		    (rgn23_data[offset + 3] != 0)) {
			offset += rgn23_data[offset + 1] * 4 + 4;
			continue;
		}

		/* Driver found a driver specific TLV in the config region */
		sub_tlv_len = rgn23_data[offset + 1] * 4;
		offset += 4;
		tlv_offset = 0;

		/*
		 * Search for configured port state sub-TLV.
		 */
		while ((offset < data_size) &&
			(tlv_offset < sub_tlv_len)) {
			if (rgn23_data[offset] == LPFC_REGION23_LAST_REC) {
				offset += 4;
				tlv_offset += 4;
				break;
			}
			if (rgn23_data[offset] != PORT_STE_TYPE) {
				offset += rgn23_data[offset + 1] * 4 + 4;
				tlv_offset += rgn23_data[offset + 1] * 4 + 4;
				continue;
			}

			/* This HBA contains PORT_STE configured */
			if (!rgn23_data[offset + 2])
				phba->hba_flag |= LINK_DISABLED;

			goto out;
		}
	}
out:
	if (pmb)
		mempool_free(pmb, phba->mbox_mem_pool);
	kfree(rgn23_data);
	return;
}

void
lpfc_cleanup_pending_mbox(struct lpfc_vport *vport)
{
	struct lpfc_hba *phba = vport->phba;
	LPFC_MBOXQ_t *mb, *nextmb;
	struct lpfc_dmabuf *mp;
	struct lpfc_nodelist *ndlp;

	spin_lock_irq(&phba->hbalock);
	list_for_each_entry_safe(mb, nextmb, &phba->sli.mboxq, list) {
		if (mb->vport != vport)
			continue;

		if ((mb->u.mb.mbxCommand != MBX_REG_LOGIN64) &&
			(mb->u.mb.mbxCommand != MBX_REG_VPI))
			continue;

		if (mb->u.mb.mbxCommand == MBX_REG_LOGIN64) {
			mp = (struct lpfc_dmabuf *) (mb->context1);
			if (mp) {
				__lpfc_mbuf_free(phba, mp->virt, mp->phys);
				kfree(mp);
			}
			ndlp = (struct lpfc_nodelist *) mb->context2;
			if (ndlp) {
				lpfc_nlp_put(ndlp);
				mb->context2 = NULL;
			}
		}
		list_del(&mb->list);
		mempool_free(mb, phba->mbox_mem_pool);
	}
	mb = phba->sli.mbox_active;
	if (mb && (mb->vport == vport)) {
		if ((mb->u.mb.mbxCommand == MBX_REG_LOGIN64) ||
			(mb->u.mb.mbxCommand == MBX_REG_VPI))
			mb->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
		if (mb->u.mb.mbxCommand == MBX_REG_LOGIN64) {
			ndlp = (struct lpfc_nodelist *) mb->context2;
			if (ndlp) {
				lpfc_nlp_put(ndlp);
				mb->context2 = NULL;
			}
			/* Unregister the RPI when mailbox complete */
			mb->mbox_flag |= LPFC_MBX_IMED_UNREG;
		}
	}
	spin_unlock_irq(&phba->hbalock);
}

