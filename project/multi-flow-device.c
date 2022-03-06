#define EXPORT_SYMTAB
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/sched.h>	
#include <linux/pid.h>
#include <linux/tty.h>
#include <linux/version.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matteo Ferretti <0300049>");

#define MODNAME "MULTI-FLOW-DEVICE"
#define DEVICE_NAME "mfdev"

#define get_major(session)	MAJOR(session->f_inode->i_rdev)
#define get_minor(session)	MINOR(session->f_inode->i_rdev)

#define OBJECT_MAX_SIZE  (4096)

typedef struct _object_state
{
   struct mutex mutex_hi;  //syncronization utilities
   struct mutex mutex_lo;
	int priorityMode;   //0 = low priority usage, 1 = high priority usage
   int blockingModeOn; //0 = non-blocking RW ops, 1 = blocking rw ops
   unsigned long awake_timeout; //timeout regulating the awake of blocking operations
   int valid_bytes_lo;  //written bytes present in the low priority flow
   int valid_bytes_hi;  //written bytes present in the high priority flow
   char * low_priority_flow;  //low priority data stream
   char * high_priority_flow; //high priority data stream
   wait_queue_head_t high_prio_queue;  //wait event queues
   wait_queue_head_t low_prio_queue;

   struct work_struct workqueue_lo;
} object_state;
typedef struct _packed_work
{
   struct file *filp;
   const char *buff;
   size_t len;
   loff_t *off;
   struct work_struct workqueue;
} packed_work;


static int Major;

#define MINORS 128
object_state objects[MINORS];

void work_function(struct work_struct *work)
{
   //Implementation of deferred work
   packed_work *info = container_of(work,packed_work,workqueue);
   int minor = get_minor(info->filp);
   int ret;
   size_t len;
   loff_t *off;
   object_state *the_object = objects + minor;

   if(the_object->blockingModeOn)
      wait_event_timeout(the_object->low_prio_queue, mutex_trylock(&(the_object->mutex_lo)),the_object->awake_timeout);
   else
      mutex_trylock(&(the_object->mutex_lo));

   off = info->off;
   len = info->len;

   

   *off += the_object->valid_bytes_lo;

   //if offset too large
   if(*off >= OBJECT_MAX_SIZE) 
   {
      //Qui sto mettendo la condizione solo sul mutex lo perchè solo quello puo essere preso nelle write
      mutex_unlock(&(the_object->mutex_lo));
      wake_up(&(the_object->low_prio_queue));
      printk("%s: ERROR - No space left on device\n",MODNAME);
	   return;
   }
   //if offset beyond the current stream size
   if((*off > the_object->valid_bytes_lo)) {
      mutex_unlock(&(the_object->mutex_lo));
      wake_up(&(the_object->low_prio_queue));
      printk("%s: ERROR - Out of stream resources\n",MODNAME);
      return;
   }
   
   if((OBJECT_MAX_SIZE - *off) < len)
      len = OBJECT_MAX_SIZE - *off;
   
  
   
   printk("%s: Sto per copiare sulla low flow a offset %lld il buffer '%s' di lunghezza %ld",MODNAME,*off,info->buff,len);
   ret = copy_from_user(&(the_object->low_priority_flow[*off]),info->buff,len);
   *off += (len - ret);
   the_object->valid_bytes_lo = *off;
   

   printk("%s: FLOWS BEFORE RETURNING\nLOWPRIOFLOW: %s\nHIGHPRIOFLOW: %s\n",MODNAME, the_object->low_priority_flow, the_object->high_priority_flow);
   printk("%s: BEFORE RETURNING OFFSET VALUES:\nnext_offset_lo = %d\nnext_offset_hi = %d\n",MODNAME,the_object->valid_bytes_lo,the_object->valid_bytes_hi); 
   mutex_unlock(&(the_object->mutex_hi));
   wake_up(&(the_object->high_prio_queue));

   return;




}



/*
      DRIVER
*/

static int dev_open(struct inode *inode, struct file *file)
{
   int minor = get_minor(file);
   if(minor >= MINORS)
   {
      printk("%s: ERROR - minor number %d out of handled range.\n",MODNAME,minor);
      return -ENODEV;
   }
   printk("%s: OPENED DEVICE FILE WITH MINOR %d\n",MODNAME,minor);
   return 0;
}

static int dev_release(struct inode *inode, struct file *file)
{
   int minor = get_minor(file);
   printk("%s: CLOSED DEVICE FILE WITH MINOR %d\n",MODNAME,minor);
   return 0;
}

static ssize_t dev_write(struct file *filp, const char *buff, size_t len, loff_t *off)
{
   //Synchronous for high and asynchronous low priority - Appena prende il lock esegue o lo lascia accodato
   //Bloccante = aspetta il lock, non bloccante lascia stare se non lo prende
   int ret;
   int minor = get_minor(filp);
   object_state *the_object = objects + minor;
   int highPriority = the_object->priorityMode;

   printk("%s: WRITE CALLED ON [MAJ-%d,MIN-%d]\n",MODNAME,get_major(filp),get_minor(filp));
   printk("%s: \nPriority=%d\n*off=%lld\nvalid_bytes_hi=%d\nvalid_bytes_lo=%d\n",MODNAME,highPriority,*off,the_object->valid_bytes_hi,the_object->valid_bytes_lo);
   
   //Se la modalità è bloccante
   if(the_object->blockingModeOn)
   {
      if(highPriority)
      {
         wait_event_timeout(the_object->high_prio_queue, mutex_trylock(&(the_object->mutex_hi)),the_object->awake_timeout);
      }
      else
      {
         //Deferred work
         packed_work * info;
         info -> filp = filp;
         info -> buff = buff;
         info -> len = len;
         info -> workqueue = the_object->workqueue_lo;
         schedule_work(&(info->workqueue));
         return 20;
      }
      
   }//Se la modalità è non bloccante
   else
   {
      if(highPriority)
      {
         mutex_trylock(&(the_object->mutex_hi));
      }
      else
      {
         //Deferred work
         packed_work * info;
         info -> filp = filp;
         info -> buff = buff;
         info -> len = len;
         info -> workqueue = the_object->workqueue_lo;
         schedule_work(&(info->workqueue));
         return 20;
      }
   }




  /*
   if(the_object->blockingModeOn)
      mutex_lock(&(the_object->mutex));
   else
      mutex_trylock(&(the_object->mutex));
   */
   if (highPriority)
      *off += the_object->valid_bytes_hi;
   else
      *off += the_object->valid_bytes_lo;

   printk("%s: L'offset è stato impostato su %lld\n",MODNAME,*off);

   //if offset too large
   if(*off >= OBJECT_MAX_SIZE) 
   {
      //Qui sto mettendo la condizione solo sul mutex hi perchè solo quello puo essere preso nelle write
      mutex_unlock(&(the_object->mutex_hi));
      wake_up(&(the_object->high_prio_queue));
      printk("%s: ERROR - No space left on device\n",MODNAME);
	   return -ENOSPC;
   }
   //if offset beyond the current stream size
   if((!highPriority && *off > the_object->valid_bytes_lo) || (highPriority && *off > the_object->valid_bytes_hi)) {
      mutex_unlock(&(the_object->mutex_hi));
      wake_up(&(the_object->high_prio_queue));
      printk("%s: ERROR - Out of stream resources\n",MODNAME);
      return -ENOSR;
   }
   
   if((OBJECT_MAX_SIZE - *off) < len)
      len = OBJECT_MAX_SIZE - *off;
   
  
   if(highPriority)
   {
      printk("%s: Sto per copiare sulla high flow a offset %lld il buffer '%s' di lunghezza %ld",MODNAME,*off,buff,len);
      ret = copy_from_user(&(the_object->high_priority_flow[*off]),buff,len);
      *off += (len - ret);
      the_object->valid_bytes_hi = *off;
   }
   else
   {
      printk("%s: Sto per copiare sulla low flow a offset %lld il buffer '%s' di lunghezza %ld",MODNAME,*off,buff,len);
      ret = copy_from_user(&(the_object->low_priority_flow[*off]),buff,len);
      *off += (len - ret);
      the_object->valid_bytes_lo = *off;
   } 

   printk("%s: FLOWS BEFORE RETURNING\nLOWPRIOFLOW: %s\nHIGHPRIOFLOW: %s\n",MODNAME, the_object->low_priority_flow, the_object->high_priority_flow);
   printk("%s: BEFORE RETURNING OFFSET VALUES:\nnext_offset_lo = %d\nnext_offset_hi = %d\n",MODNAME,the_object->valid_bytes_lo,the_object->valid_bytes_hi); 
   mutex_unlock(&(the_object->mutex_hi));
   wake_up(&(the_object->high_prio_queue));

   return len - ret;
}

static ssize_t dev_read(struct file *filp, char *buff, size_t len, loff_t *off)
{
   //Synchronous for high priority, synchronous for low priority aspetta il lock e poi esegue subito
   //Bloccante aspetta il lock, non bloccante non aspetta e rida controllo all'user
   int minor = get_minor(filp);
   int ret;
   object_state *the_object = objects + minor;
   int highPriority = the_object->priorityMode;
   
   printk("%s: READ CALLED ON [MAJ-%d,MIN-%d]\n",MODNAME,get_major(filp),get_minor(filp));

   if(the_object->blockingModeOn)
   {
      if(highPriority)
      {
         wait_event_timeout(the_object->high_prio_queue, mutex_trylock(&(the_object->mutex_hi)),the_object->awake_timeout);
      }
      else
      {
         wait_event_timeout(the_object->low_prio_queue, mutex_trylock(&(the_object->mutex_lo)),the_object->awake_timeout);
      }
      
   }//Se la modalità è non bloccante
   else
   {
      if(highPriority)
      {
         mutex_trylock(&(the_object->mutex_hi));
      }
      else
      {
         mutex_trylock(&(the_object->mutex_lo));
      }
   }


/*
   if(the_object->blockingModeOn)
      mutex_lock(&(the_object->mutex));
   else
      mutex_trylock(&(the_object->mutex));  
   */
   if((!highPriority && *off > the_object->valid_bytes_lo)  )
   {
      mutex_unlock(&(the_object->mutex_lo));
      wake_up(&(the_object->low_prio_queue));
	   return 0;
   }
   else if((highPriority && *off > the_object->valid_bytes_hi) )
   {
      mutex_unlock(&(the_object->mutex_hi));
      wake_up(&(the_object->high_prio_queue));
	   return 0;
   } 
   if((!highPriority && the_object->valid_bytes_lo - *off < len) ) 
      len = the_object->valid_bytes_lo - *off;
   else if((highPriority && the_object->valid_bytes_hi - *off < len))
      len = the_object->valid_bytes_hi - *off;
   

   if(highPriority)
   {
      ret = copy_to_user(buff,&(the_object->high_priority_flow[*off]),len);
      the_object->high_priority_flow += len;
      the_object->valid_bytes_hi -= len;

      printk("%s: lettura effettuata, gli stream ora sono:\nhigh: %s\nlow: %s\n",MODNAME,the_object->high_priority_flow,the_object->low_priority_flow);
      mutex_unlock(&(the_object->mutex_hi));
      wake_up(&(the_object->high_prio_queue));
   }
   else
   {
      ret = copy_to_user(buff,&(the_object->low_priority_flow[*off]),len);
      the_object->low_priority_flow += len;
      the_object->valid_bytes_lo -= len;

      printk("%s: lettura effettuata, gli stream ora sono:\nhigh: %s\nlow: %s\n",MODNAME,the_object->high_priority_flow,the_object->low_priority_flow);
      mutex_unlock(&(the_object->mutex_lo));
      wake_up(&(the_object->low_prio_queue));
   }


   return len - ret;
}

static long dev_ioctl(struct file *filp, unsigned int command, unsigned long param)
{
   int minor = get_minor(filp);
   object_state *the_object;

   the_object = objects + minor;
   printk("%s: IOCTL CALLED ON [MAJ-%d,MIN-%d] - command = %u \n",MODNAME,get_major(filp),get_minor(filp),command);
   
   //Device state control

   if(command == 0)
      the_object->priorityMode = 0;
   else if(command == 1)
      the_object->priorityMode = 1;
   else if(command == 2)
      the_object->blockingModeOn = 0;
   else if(command == 3)
      the_object->blockingModeOn = 1;
   else if (command == 4)
      the_object->awake_timeout = param;
   
   return 0;
}

static struct file_operations fops = {
  .owner = THIS_MODULE,
  .write = dev_write,
  .read = dev_read,
  .open =  dev_open,
  .release = dev_release,
  .unlocked_ioctl = dev_ioctl
};

int init_module(void)
{
   int i;
	//Driver internal state initialization
	for(i=0;i<MINORS;i++)
   {
		mutex_init(&(objects[i].mutex_hi));
      mutex_init(&(objects[i].mutex_lo));
      init_waitqueue_head(&(objects[i].high_prio_queue));
      init_waitqueue_head(&(objects[i].low_prio_queue));

      INIT_WORK(&(objects[i].workqueue_lo),work_function);

      objects[i].awake_timeout=500;
      objects[i].blockingModeOn=0;
      objects[i].priorityMode=0;
		objects[i].valid_bytes_hi = 0;
      objects[i].valid_bytes_lo = 0;
      objects[i].low_priority_flow = NULL;
		objects[i].low_priority_flow = (char*)__get_free_page(GFP_KERNEL);
      objects[i].high_priority_flow = NULL;
		objects[i].high_priority_flow = (char*)__get_free_page(GFP_KERNEL);
		if(objects[i].low_priority_flow == NULL || objects[i].high_priority_flow == NULL) goto revert_allocation;
	}

	Major = __register_chrdev(0, 0, 128, DEVICE_NAME, &fops);
	//actually allowed minors are directly controlled within this driver

	if (Major < 0) 
   {
	  printk("%s: ERROR - device registration failed\n",MODNAME);
	  return Major;
	}
	printk(KERN_INFO "%s: DEVICE REGISTERED - Assigned MAJOR = %d\n",MODNAME, Major);
	return 0;

revert_allocation:
	for(;i>=0;i--)
   {
		free_page((unsigned long)objects[i].low_priority_flow);
      free_page((unsigned long)objects[i].high_priority_flow);
	}
   printk("%s: ERROR - Requested memory is not available\n",MODNAME);
	return -ENOMEM;
}

void cleanup_module(void)
{
	int i;
	for(i=0;i<MINORS;i++)
   {
		free_page((unsigned long)objects[i].low_priority_flow);
		free_page((unsigned long)objects[i].high_priority_flow);
	}
	unregister_chrdev(Major, DEVICE_NAME);
	printk(KERN_INFO "%s: DEVICE WITH MAJOR = %d WAS SUCCESSFULLY UNREGISTERED\n",MODNAME, Major);
	return;
}
